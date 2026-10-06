#!/usr/bin/env python3
"""Headroom -- a tiny always-on-top free-disk-space widget for data-acquisition PCs.

For every drive (or the ones you pick) it shows free space, the current write
rate and the time left until the disk is full, and turns amber / flashing red
(with a beep) before a recording can run out of space.

* Python 3.8+ standard library only (Tk).  Windows, Linux, macOS.
* Near-zero load: never scans files -- one statvfs()/GetDiskFreeSpaceEx() call
  per drive per interval reads the filesystem's in-memory free-space counter.
  One timer wakeup per interval, redraws only when a shown value changes, and
  runs at the lowest CPU priority.  Network/USB drives are queried on a worker
  thread so a hung share can never freeze the widget.

    Windows:  pythonw headroom.py      (no console window)
    Linux:    python3 headroom.py      (Debian/Ubuntu: sudo apt install python3-tk)

Drag with the left mouse button; right-click for drives, alerts, size, etc.
Settings are kept in headroom.json; a headroom.json placed next to this
script is used instead (portable, pre-configured copies for several rigs).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from collections import deque, namedtuple
from concurrent.futures import ThreadPoolExecutor

try:
    import tkinter as tk
    import tkinter.font as tkfont
    from tkinter import filedialog
except ImportError:  # pragma: no cover
    sys.exit("Headroom needs Tk.  On Debian/Ubuntu: sudo apt install python3-tk")

APP, VERSION = "Headroom", "1.0"
IS_WIN = os.name == "nt"
IS_MAC = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")

MiB, GiB, TiB, PiB = 1 << 20, 1 << 30, 1 << 40, 1 << 50
IDLE_RATE = 256 * 1024          # bytes/s; a slower net change counts as "idle"

NA, OK, WARN, CRIT = -1, 0, 1, 2
BG, BORDER, FG, DIM, BAR_BG, FLASH_BG = (
    "#1b1f24", "#444c56", "#e6edf3", "#8b949e", "#373e47", "#7a1d18")
LEVEL_COLOR = {NA: DIM, OK: "#3fb950", WARN: "#e3b341", CRIT: "#ff6b61"}

DEFAULTS = {
    "drives": [],              # [] = every local drive (auto); or e.g. ["C:", "D:"], ["/", "/data"]
    "include_network": False,  # auto mode: also show network shares
    "interval_s": 2.0,         # seconds between measurements
    "warn_gb": 100.0,          # amber below this much free space (GB)
    "crit_gb": 20.0,           # red + alarm below this much free space (GB)
    "warn_min": 60.0,          # amber when the disk will be full within N minutes (0 = off)
    "crit_min": 15.0,          # red + alarm when full within N minutes (0 = off)
    "rate_window_s": 30.0,     # write rate = sustained trend over this many seconds
    "show_rate": True,         # show write rate and time left
    "sound": True,             # beep while any drive is red
    "alarm_repeat_s": 30.0,    # seconds between beeps
    "font_size": 11,
    "opacity": 0.92,
    "x": None,                 # widget position (None = bottom-right corner)
    "y": None,
}
LIMITS = {"interval_s": (0.5, 3600), "warn_gb": (0, 1e9), "crit_gb": (0, 1e9),
          "warn_min": (0, 1e6), "crit_min": (0, 1e6), "rate_window_s": (5, 3600),
          "alarm_repeat_s": (2, 86400), "font_size": (6, 72), "opacity": (0.2, 1.0)}

# kind: local | removable | network | folder;  fast: probe inline (local kernel filesystem)
Drive = namedtuple("Drive", "path name kind fast")


# ------------------------------------------------------------------ config --

def default_config_path():
    here = os.path.dirname(os.path.abspath(
        sys.executable if getattr(sys, "frozen", False) else sys.argv[0]))
    portable = os.path.join(here, "headroom.json")
    if os.path.exists(portable):
        return portable
    if IS_WIN:
        base = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), APP)
    elif IS_MAC:
        base = os.path.expanduser("~/Library/Application Support/" + APP)
    else:
        base = os.path.join(os.environ.get("XDG_CONFIG_HOME")
                            or os.path.expanduser("~/.config"), "headroom")
    return os.path.join(base, "headroom.json")


def load_config(path):
    """Return (config, error).  Unknown keys and bad values fall back to defaults."""
    cfg = dict(DEFAULTS)
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        return cfg, None
    except (OSError, ValueError) as e:
        return cfg, f"{os.path.basename(path)}: {e}"
    if not isinstance(data, dict):
        return cfg, f"{os.path.basename(path)}: expected a JSON object"
    for key, v in data.items():        # keep keys used by the Windows / GNOME versions
        if key not in DEFAULTS:
            cfg[key] = v
    for key, dflt in DEFAULTS.items():
        v = data.get(key, dflt)
        num = isinstance(v, (int, float)) and not isinstance(v, bool)
        if key == "drives":
            if isinstance(v, str):
                v = [v]
            if isinstance(v, list) and all(isinstance(s, str) for s in v):
                cfg[key] = [s.strip() for s in v if s.strip()]
        elif key in ("x", "y"):
            cfg[key] = int(v) if num else None
        elif isinstance(dflt, bool):
            if isinstance(v, bool):
                cfg[key] = v
        elif num:
            lo, hi = LIMITS[key]
            cfg[key] = type(dflt)(min(max(v, lo), hi))
    return cfg, None


def save_config(path, cfg):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


# -------------------------------------------------------------- formatting --

def fmt_size(n):
    v, unit = n / MiB, "MB"
    for div, u in ((GiB, "GB"), (TiB, "TB"), (PiB, "PB")):
        if v < 999.5:
            break
        v, unit = n / div, u
    return f"{v:.2f} {unit}" if v < 9.995 else f"{v:.1f} {unit}" if v < 99.95 else f"{v:.0f} {unit}"


def fmt_rate(r):
    v, unit = r / MiB, "MB/s"
    if v >= 999.5:
        v, unit = r / GiB, "GB/s"
    return f"{v:.1f} {unit}" if v < 99.95 else f"{v:.0f} {unit}"


def fmt_eta(seconds):
    s = int(seconds)
    if s < 60:
        return f"{s}s"
    m, s = divmod(s, 60)
    if m < 10:
        return f"{m}m{s:02d}s"
    if m < 60:
        return f"{m}m"
    h, m = divmod(m, 60)
    if h < 48:
        return f"{h}h{m:02d}m"
    d, h = divmod(h, 24)
    return f"{d}d{h}h" if d < 100 else ">99d"


def short_path(p, n=22):
    if len(p) <= n:
        return p
    parts = [s for s in p.replace("\\", "/").split("/") if s]
    s = "…/" + "/".join(parts[-2:])
    return s if len(s) <= n else "…" + s[-(n - 1):]


# ------------------------------------------------------------ measurement --

def _slope(pts):
    n = len(pts)
    mt = sum(t for t, _ in pts) / n
    mf = sum(f for _, f in pts) / n
    sxx = sum((t - mt) ** 2 for t, _ in pts)
    return sum((t - mt) * (f - mf) for t, f in pts) / sxx if sxx > 0 else None


def write_rate(hist):
    """Sustained space consumption in bytes/s over the window [(t, free), ...].

    Median of the least-squares trends of the window's three thirds, so a recording
    shows its true rate while a one-off file copy doesn't look like one.
    """
    if len(hist) < 5 or hist[-1][0] - hist[0][0] < 3:
        return None
    h = list(hist)
    i, j = len(h) // 3, 2 * len(h) // 3
    slopes = [_slope(h[:i + 1]), _slope(h[i:j + 1]), _slope(h[j:])]
    return None if None in slopes else -sorted(slopes)[1]


def eta_level(seconds, cfg):
    minutes = seconds / 60
    return CRIT if minutes < cfg["crit_min"] else WARN if minutes < cfg["warn_min"] else OK


def level_of(free, total, rate, cfg):
    if total <= 0:
        return NA
    gb = free / GiB
    lvl = CRIT if gb < cfg["crit_gb"] else WARN if gb < cfg["warn_gb"] else OK
    if rate is not None and rate > IDLE_RATE:
        lvl = max(lvl, eta_level(free / rate, cfg))
    return lvl


def probe(path, want_label):
    """Runs in a worker thread: (time, total, free, label)."""
    total, _, free = shutil.disk_usage(path)
    t = time.monotonic()
    return t, total, free, (volume_label(path) if want_label else None)


def describe_error(e):
    if IS_WIN and getattr(e, "winerror", None) == 21:
        return "no media"
    if isinstance(e, FileNotFoundError):
        return "not found"
    if isinstance(e, PermissionError):
        return "no access"
    return "unavailable"


# ---------------------------------------------------------------- platform --

if IS_WIN:
    import ctypes
    from ctypes import wintypes

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _u32 = ctypes.WinDLL("user32", use_last_error=True)

    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                    ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]

    def _sig(fn, res, *args):
        fn.restype, fn.argtypes = res, args

    _sig(_k32.GetLogicalDriveStringsW, wintypes.DWORD, wintypes.DWORD, wintypes.LPWSTR)
    _sig(_k32.GetDriveTypeW, wintypes.UINT, wintypes.LPCWSTR)
    _sig(_k32.GetVolumeInformationW, wintypes.BOOL, wintypes.LPCWSTR, wintypes.LPWSTR,
         wintypes.DWORD, wintypes.LPDWORD, wintypes.LPDWORD, wintypes.LPDWORD,
         wintypes.LPWSTR, wintypes.DWORD)
    _sig(_k32.SetErrorMode, wintypes.UINT, wintypes.UINT)
    _sig(_k32.GetLogicalDrives, wintypes.DWORD)
    _sig(_k32.GetCurrentProcess, wintypes.HANDLE)
    _sig(_k32.SetPriorityClass, wintypes.BOOL, wintypes.HANDLE, wintypes.DWORD)
    _sig(_k32.CreateMutexW, wintypes.HANDLE, ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR)
    if hasattr(_k32, "SetProcessInformation"):       # Windows 8+
        _sig(_k32.SetProcessInformation, wintypes.BOOL, wintypes.HANDLE, ctypes.c_int,
             ctypes.c_void_p, wintypes.DWORD)
    _sig(_u32.MonitorFromRect, wintypes.HANDLE, ctypes.POINTER(wintypes.RECT), wintypes.DWORD)
    _sig(_u32.MonitorFromPoint, wintypes.HANDLE, wintypes.POINT, wintypes.DWORD)
    _sig(_u32.GetMonitorInfoW, wintypes.BOOL, wintypes.HANDLE, ctypes.POINTER(MONITORINFO))
    _sig(_u32.GetSystemMetrics, ctypes.c_int, ctypes.c_int)

    # Never pop up "There is no disk in the drive" dialogs while probing.
    _k32.SetErrorMode(0x0001 | 0x8000)   # SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX

    def _win_area(hmon):
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(MONITORINFO)
        if hmon and _u32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
            a = mi.rcWork
            return a.left, a.top, a.right, a.bottom
        return 0, 0, _u32.GetSystemMetrics(0), _u32.GetSystemMetrics(1)


def volume_label(path):
    if not (IS_WIN and len(path) == 3 and path[1:] == ":\\"):
        return None
    buf = ctypes.create_unicode_buffer(261)
    ok = _k32.GetVolumeInformationW(path, buf, len(buf), None, None, None, None, 0)
    return buf.value if ok else ""


def norm_key(p):
    p = p.strip()
    if IS_WIN:
        if len(p) == 2 and p[1] == ":":
            p += "\\"
        return os.path.normcase(os.path.normpath(p))
    return os.path.normpath(p)


_FAST_FS = {"ext2", "ext3", "ext4", "xfs", "btrfs", "zfs", "f2fs", "jfs", "reiserfs",
            "bcachefs", "nilfs2", "ntfs3", "exfat", "vfat", "msdos", "hfs", "hfsplus",
            "apfs", "udf"}
_LOCAL_FS = _FAST_FS | {"fuseblk", "ntfs"}      # FUSE drivers can stall under heavy writes
_NET_FS = {"nfs", "nfs4", "cifs", "smb3", "smbfs", "9p", "ceph", "glusterfs", "lustre",
           "beegfs", "gpfs", "fuse.sshfs", "fuse.rclone", "fuse.glusterfs", "fuse.s3fs"}
_SKIP_MOUNTS = ("/proc", "/sys", "/dev", "/run", "/boot", "/efi", "/snap", "/var/snap",
                "/var/lib/docker", "/var/lib/containers", "/var/lib/kubelet")


def _unoct(s):
    return re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), s)


def _discover_linux():
    out, seen = [], set()
    with open("/proc/self/mounts", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            src, mp, fs = _unoct(parts[0]), _unoct(parts[1]), parts[2]
            kind = "local" if fs in _LOCAL_FS else "network" if fs in _NET_FS else None
            if kind is None or src in seen:
                continue
            if mp != "/" and not mp.startswith("/run/media/") and any(
                    mp == p or mp.startswith(p + "/") for p in _SKIP_MOUNTS):
                continue
            seen.add(src)
            out.append(Drive(mp, short_path(mp), kind, fs in _FAST_FS))
    return out


def _discover_mac():
    out = [Drive("/", "/", "local", True)]
    try:
        txt = subprocess.run(["/sbin/mount"], capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.SubprocessError):
        return out
    for line in txt.splitlines():
        m = re.match(r"^(.*) on (.*) \((.*)\)$", line)
        if not m or not m.group(2).startswith("/Volumes/"):
            continue
        opts = [o.strip() for o in m.group(3).split(",")]
        if "nobrowse" not in opts:
            local = "local" in opts
            out.append(Drive(m.group(2), os.path.basename(m.group(2)),
                             "local" if local else "network", local))
    return out


def _discover_windows():
    buf = ctypes.create_unicode_buffer(1024)
    n = _k32.GetLogicalDriveStringsW(len(buf), buf)
    out = []
    for root in buf[:n].split("\0"):
        kind = {2: "removable", 3: "local", 4: "network", 6: "local"}.get(
            _k32.GetDriveTypeW(root)) if root else None
        if kind:
            out.append(Drive(root, root[:2], kind, kind == "local"))
    return out


def discover(include_network):
    try:
        if IS_WIN:
            found = _discover_windows()
        elif IS_MAC:
            found = _discover_mac()
        else:
            found = _discover_linux()
    except Exception:
        traceback.print_exc()
        found = [Drive(os.path.abspath(os.sep), os.path.abspath(os.sep), "local", False)]
    return [d for d in found if include_network or d.kind != "network"]


def mounts_signature():
    try:
        if IS_WIN:
            return _k32.GetLogicalDrives()
        if IS_MAC:
            return tuple(sorted(os.listdir("/Volumes")))
        with open("/proc/self/mounts", "rb") as f:
            return f.read()
    except OSError:
        return None


def wanted_drives(cfg):
    if not cfg["drives"]:
        return discover(cfg["include_network"])
    known = {norm_key(d.path): d for d in discover(True)}
    out = []
    for p in cfg["drives"]:
        d = known.get(norm_key(p))
        out.append(d or Drive(p + "\\" if IS_WIN and len(p) == 2 and p[1] == ":" else p,
                              short_path(p), "folder", False))
    return out


def linux_monitors():
    """[(x0, y0, x1, y1, primary)] from xrandr, or [] if unavailable."""
    exe = shutil.which("xrandr")
    if not exe or not os.environ.get("DISPLAY"):
        return []
    try:
        out = subprocess.run([exe, "--listactivemonitors"], capture_output=True,
                             text=True, timeout=2).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    mons = []
    for line in out.splitlines():
        m = re.match(r"\s*\d+:\s+\+?(\*?)\S+\s+(\d+)/\d+x(\d+)/\d+([+-]\d+)([+-]\d+)", line)
        if m:
            w, h, x, y = (int(g) for g in m.group(2, 3, 4, 5))
            mons.append((x, y, x + w, y + h, m.group(1) == "*"))
    return mons


def find_sound_cmd():
    if IS_WIN:
        return None
    if IS_MAC:
        snd = "/System/Library/Sounds/Sosumi.aiff"
        return ["afplay", snd] if os.path.exists(snd) else None
    sounds = "/usr/share/sounds/freedesktop/stereo/"
    snd = next((sounds + f for f in ("alarm-clock-elapsed.oga", "dialog-warning.oga",
                                     "bell.oga") if os.path.exists(sounds + f)), None)
    for player in ("paplay", "pw-play"):
        exe = shutil.which(player)
        if exe and snd:
            return [exe, snd]
    exe = shutil.which("canberra-gtk-play")
    return [exe, "-i", "alarm-clock-elapsed"] if exe else None


def _win_beeps():
    import winsound
    try:
        for freq in (1400, 1000, 1400):
            winsound.Beep(freq, 180)
    except RuntimeError:
        winsound.MessageBeep(winsound.MB_ICONHAND)


# --------------------------------------------------------------- autostart --

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


def _autostart_file():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "autostart", "headroom.desktop")


def _desktop_quote(a):
    a = a.replace("%", "%%")
    if re.fullmatch(r"[\w@%+=:,./-]+", a):
        return a
    a = a.replace("\\", "\\" * 4)
    return '"' + re.sub(r'(["`$])', r"\\\\\1", a) + '"'


def autostart_enabled():
    if IS_WIN:
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
                winreg.QueryValueEx(k, APP)
            return True
        except OSError:
            return False
    return IS_LINUX and os.path.exists(_autostart_file())


def set_autostart(on, argv):
    if IS_WIN:
        import winreg
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            if on:
                winreg.SetValueEx(k, APP, 0, winreg.REG_SZ, subprocess.list2cmdline(argv))
            else:
                try:
                    winreg.DeleteValue(k, APP)
                except FileNotFoundError:
                    pass
    elif IS_LINUX:
        path = _autostart_file()
        if on:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write("[Desktop Entry]\nType=Application\nName=Headroom\n"
                        "Comment=Always-on-top free disk space monitor\n"
                        f"Exec={' '.join(_desktop_quote(a) for a in argv)}\n"
                        "Terminal=false\nX-GNOME-Autostart-enabled=true\n")
        elif os.path.exists(path):
            os.remove(path)


# --------------------------------------------------------------------- app --

class Vol:
    __slots__ = ("path", "name", "kind", "fast", "label", "label_tried", "total", "free",
                 "err", "hist", "future", "sent", "rate", "level", "items")

    def __init__(self, d):
        self.path, self.name, self.kind, self.fast = d
        self.label, self.label_tried = "", False
        self.total = self.free = 0
        self.err = None
        self.hist = deque()           # (monotonic time, free bytes)
        self.future = None            # in-flight probe
        self.sent = 0.0
        self.rate = None              # bytes/s being consumed
        self.level = NA
        self.items = {}

    @property
    def title(self):
        return f"{self.name}  {self.label[:14]}" if self.label else self.name

    @property
    def eta(self):
        return self.free / self.rate if self.rate and self.rate > IDLE_RATE else None


class App:
    def __init__(self, cfg_path, cfg, cfg_error=None):
        self.cfg_path, self.cfg, self.cfg_error = cfg_path, cfg, cfg_error
        self.cfg_mtime = self._mtime()
        self.vols, self.shown = [], []
        self.pool = ThreadPoolExecutor(max_workers=8, thread_name_prefix="probe")
        self.crit_since, self.last_beep = None, float("-inf")
        self.flash_on, self.flash_job, self.tick_job = False, None, None
        self.drag, self.menu, self.mvars, self.modal = None, None, [], False
        self.pos, self.last, self.hk_time = (0, 0), {}, time.monotonic()
        self.mount_sig = mounts_signature()
        self.sound_cmd = find_sound_cmd()

        self.root = root = tk.Tk(className=APP)
        root.withdraw()
        root.title(APP)
        root.overrideredirect(True)
        root.report_callback_exception = self.on_error
        root.protocol("WM_DELETE_WINDOW", self.quit)
        self.scale = max(1.0, root.winfo_fpixels("1i") / 96.0)
        self.family = tkfont.nametofont("TkDefaultFont").actual("family")
        self.font, self.bold = tkfont.Font(root), tkfont.Font(root)
        self.set_fonts()
        self.cv = cv = tk.Canvas(root, bg=BG, bd=0, highlightthickness=0)
        cv.pack()
        cv.bind("<ButtonPress-1>", self.drag_start)
        cv.bind("<B1-Motion>", self.drag_move)
        cv.bind("<ButtonRelease-1>", self.drag_end)
        for seq in (("<Button-2>", "<Control-Button-1>") if IS_MAC else ("<Button-3>",)):
            cv.bind(seq, self.show_menu)
        if not IS_WIN:
            for s in (signal.SIGINT, signal.SIGTERM):
                signal.signal(s, lambda *_: self.quit())

        self.sync_volumes()
        self.relayout(clamp=False)
        self.place(cfg["x"], cfg["y"])
        root.deiconify()
        self.apply_window_attrs()
        self.tick()

    # -- sampling ----------------------------------------------------------

    def tick(self):
        """The only periodic wakeup: measure, redraw what changed, housekeeping."""
        now = time.monotonic()
        relayout = waiting = False
        for v in self.vols:
            want_label = IS_WIN and not v.label_tried
            if v.fast:                      # local disk: a few microseconds, do it inline
                v.label_tried = True
                try:
                    res = probe(v.path, want_label)
                except Exception as e:  # noqa: BLE001 - any failure just marks the drive
                    res = e
                relayout |= self.store(v, res)
            else:                           # network/USB/folder: never block the UI on it
                if v.future is None:
                    v.label_tried = True
                    v.sent, v.future = now, self.pool.submit(probe, v.path, want_label)
                waiting = True
        self.harvest(relayout)
        if waiting:
            self.root.after(150, self.harvest)    # results normally arrive in < 1 ms
        if now - self.hk_time >= 5:
            self.hk_time = now
            self.housekeeping()
        self.tick_job = self.root.after(int(self.cfg["interval_s"] * 1000), self.tick)

    def store(self, v, res):
        """Apply a probe result (or exception) to v; True if the layout must change."""
        if isinstance(res, BaseException):
            v.err, v.rate = describe_error(res), None
            v.hist.clear()
            return False
        t, total, free, label = res
        v.err, v.total, v.free = None, total, free
        v.hist.append((t, free))
        while t - v.hist[0][0] > self.cfg["rate_window_s"]:
            v.hist.popleft()
        v.rate = write_rate(v.hist)
        if label is not None and label != v.label:
            v.label = label
            return True
        return False

    def harvest(self, relayout=False):
        for v in self.vols:
            f = v.future
            if f is not None and f.done():
                v.future = None
                try:
                    res = f.result()
                except Exception as e:  # noqa: BLE001
                    res = e
                relayout |= self.store(v, res)
        self.evaluate()
        if relayout or self.visible() != self.shown:
            self.relayout()
        else:
            self.render()

    def evaluate(self):
        now = time.monotonic()
        for v in self.vols:
            v.level = NA if v.err else level_of(v.free, v.total, v.rate, self.cfg)
        if any(v.level == CRIT for v in self.visible()):
            if self.crit_since is None:
                self.crit_since = now
            confirmed = now - self.crit_since >= max(3.0, 1.5 * self.cfg["interval_s"])
            if (confirmed and self.cfg["sound"]
                    and now - self.last_beep >= self.cfg["alarm_repeat_s"]):
                self.last_beep = now
                self.beep()
            if self.flash_job is None:
                self.flash()
        else:
            self.crit_since, self.last_beep = None, float("-inf")

    def flash(self):
        if any(v.level == CRIT for v in self.shown):
            self.flash_on = not self.flash_on
            self.flash_job = self.root.after(500, self.flash)
        else:
            self.flash_on, self.flash_job = False, None
        self.render()

    def beep(self):
        try:
            if IS_WIN:
                threading.Thread(target=_win_beeps, daemon=True).start()
            elif self.sound_cmd:
                subprocess.Popen(self.sound_cmd, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)
            else:
                self.root.bell()
        except Exception:  # noqa: BLE001
            self.root.bell()

    # -- drawing -----------------------------------------------------------

    def visible(self):
        auto = not self.cfg["drives"]       # hide empty card readers etc. in auto mode
        return [v for v in self.vols if not (auto and v.kind == "removable" and v.err)]

    def set_fonts(self):
        size = int(self.cfg["font_size"])
        self.font.configure(family=self.family, size=size, weight="normal")
        self.bold.configure(family=self.family, size=size, weight="bold")

    def relayout(self, clamp=True):
        cv, f, b, s = self.cv, self.font, self.bold, self.scale
        cv.delete("all")
        self.last = {}
        self.shown = shown = self.visible()
        show_rate = self.cfg["show_rate"]
        padx, pady, gap = round(10 * s), round(5 * s), round(12 * s)
        lh = max(f.metrics("linespace"), b.metrics("linespace"))
        rh = lh + round(4 * s)
        bar_h = max(4, round(lh * 0.45))
        bar_w = f.measure("0") * 10
        name_w = max([b.measure(v.title) for v in shown] + [b.measure("C:")])
        free_w = max(b.measure(t) for t in ("8.88 GB", "88.8 GB", "888 MB"))
        unit_w = f.measure(" free" if show_rate else " unavailable")
        x_bar = padx + name_w + gap
        x_free = x_bar + bar_w + gap + free_w           # right edge of the number
        x_end = x_free + unit_w
        x_rate = x_eta = 0
        if show_rate:
            x_rate = x_end + gap + f.measure("888 MB/s")    # right edge
            x_eta = x_rate + gap
            x_end = x_eta + f.measure("88h88m left")
        width = x_end + padx
        if not shown:
            msg = "No drives - right-click to choose"
            width = max(width, f.measure(msg) + 2 * padx)
            cv.create_text(padx, pady + rh / 2, anchor="w", font=f, fill=DIM, text=msg)
        height = 2 * pady + rh * max(1, len(shown))
        for i, v in enumerate(shown):
            y0 = pady + i * rh
            yc = y0 + rh / 2
            it = v.items = {}
            it["bg"] = cv.create_rectangle(0, y0, width, y0 + rh, width=0, fill=BG)
            it["name"] = cv.create_text(padx, yc, anchor="w", font=b, fill=FG, text=v.title)
            by0, by1 = round(yc - bar_h / 2), round(yc + bar_h / 2)
            cv.create_rectangle(x_bar, by0, x_bar + bar_w, by1, width=0, fill=BAR_BG)
            it["bar"] = cv.create_rectangle(x_bar, by0, x_bar, by1, width=0, fill=DIM)
            it["bar_geo"] = (x_bar, by0, bar_w, by1)
            it["free"] = cv.create_text(x_free, yc, anchor="e", font=b, fill=DIM, text="…")
            it["unit"] = cv.create_text(x_free, yc, anchor="w", font=f, fill=DIM, text=" free")
            if show_rate:
                it["rate"] = cv.create_text(x_rate, yc, anchor="e", font=f, fill=DIM, text="")
                it["eta"] = cv.create_text(x_eta, yc, anchor="w", font=f, fill=FG, text="")
        cv.create_rectangle(0, 0, width - 1, height - 1, outline=BORDER)
        cv.config(width=width, height=height)
        self.render()
        if clamp:
            self.keep_on_screen()

    def upd(self, item, **kw):
        last = self.last.setdefault(item, {})
        diff = {k: v for k, v in kw.items() if last.get(k) != v}
        if diff:
            self.cv.itemconfigure(item, **diff)
            last.update(diff)

    def render(self):
        now = time.monotonic()
        stale_after = max(3 * self.cfg["interval_s"], 6.0)
        for v in self.shown:
            it = v.items
            status = None
            if v.err or v.total <= 0:
                free_t, col, frac = ("—" if v.err else "…"), DIM, 0.0
                rate_t, eta_t, eta_c, status = "", "", DIM, v.err
            else:
                col = LEVEL_COLOR[v.level]
                free_t, frac = fmt_size(v.free), 1.0 - v.free / v.total
                eta = v.eta
                if eta is not None:
                    rate_t, eta_t = fmt_rate(v.rate), fmt_eta(eta) + " left"
                    el = eta_level(eta, self.cfg)
                    eta_c = LEVEL_COLOR[el] if el > OK else FG
                else:
                    rate_t, eta_t, eta_c = "idle", "", FG
            if v.future is not None and now - v.sent > stale_after:
                col, rate_t, eta_t, eta_c, status = DIM, "", "", DIM, "no response"
            self.upd(it["free"], text=free_t, fill=col)
            x0, y0, w, y1 = it["bar_geo"]
            coords = (x0, y0, x0 + round(w * min(max(frac, 0.0), 1.0)), y1)
            if self.last.get(("coords", it["bar"])) != coords:
                self.cv.coords(it["bar"], *coords)
                self.last[("coords", it["bar"])] = coords
            self.upd(it["bar"], fill=col)
            if "eta" in it:
                self.upd(it["unit"], text=" free")
                self.upd(it["rate"], text=rate_t, fill=DIM)
                self.upd(it["eta"], text=status or eta_t, fill=DIM if status else eta_c)
            else:
                self.upd(it["unit"], text=" " + status if status else " free")
            flash = v.level == CRIT and self.flash_on
            self.upd(it["bg"], fill=FLASH_BG if flash else BG)

    # -- window --------------------------------------------------------------

    def apply_window_attrs(self):
        try:
            self.root.attributes("-alpha", float(self.cfg["opacity"]))
        except tk.TclError:
            pass
        self.root.attributes("-topmost", True)

    def raise_top(self):
        if not IS_LINUX:                    # re-enter the top of the "topmost" band
            self.root.attributes("-topmost", True)
        self.root.lift()                    # X11: plain XRaiseWindow

    def busy(self):
        return bool(self.drag or self.modal or (self.menu and self.menu.winfo_ismapped()))

    def work_area(self, x, y, w, h):
        """(x0, y0, x1, y1) of the monitor nearest to the given rectangle."""
        if IS_WIN:
            r = wintypes.RECT(int(x), int(y), int(x + w), int(y + h))
            return _win_area(_u32.MonitorFromRect(ctypes.byref(r), 2))   # DEFAULTTONEAREST
        mons = linux_monitors() if IS_LINUX else []
        if mons:
            cx, cy = x + w / 2, y + h / 2
            return min(mons, key=lambda m: max(m[0] - cx, 0, cx - m[2]) ** 2
                       + max(m[1] - cy, 0, cy - m[3]) ** 2)[:4]
        return 0, 0, self.root.winfo_screenwidth(), self.root.winfo_screenheight()

    def primary_area(self):
        if IS_WIN:
            return _win_area(_u32.MonitorFromPoint(wintypes.POINT(0, 0), 1))  # PRIMARY
        mons = linux_monitors() if IS_LINUX else []
        for m in mons:
            if m[4]:
                return m[:4]
        if mons:
            return mons[0][:4]
        return 0, 0, self.root.winfo_screenwidth(), self.root.winfo_screenheight()

    def size(self):
        self.root.update_idletasks()
        return self.root.winfo_reqwidth(), self.root.winfo_reqheight()

    def move(self, x, y):
        self.pos = (int(x), int(y))
        self.root.geometry(f"+{self.pos[0]}+{self.pos[1]}")

    def clamp(self, x, y, w, h):
        x0, y0, x1, y1 = self.work_area(x, y, w, h)
        return min(max(x, x0), x1 - w), min(max(y, y0), y1 - h)

    def place(self, x=None, y=None):
        w, h = self.size()
        if x is None or y is None:
            x0, y0, x1, y1 = self.primary_area()
            m = round(16 * self.scale)
            x, y = x1 - w - m, y1 - h - (m if IS_WIN else round(48 * self.scale))
        else:
            x, y = self.clamp(x, y, w, h)
        self.move(x, y)

    def keep_on_screen(self):
        w, h = self.size()
        x, y = self.clamp(*self.pos, w, h)
        if (x, y) != self.pos:
            self.move(x, y)

    def drag_start(self, e):
        self.drag = (e.x_root - self.pos[0], e.y_root - self.pos[1])

    def drag_move(self, e):
        if self.drag:
            self.move(e.x_root - self.drag[0], e.y_root - self.drag[1])

    def drag_end(self, _e):
        if self.drag:
            self.drag = None
            self.keep_on_screen()
            self.set_cfg(x=self.pos[0], y=self.pos[1])

    # -- settings ------------------------------------------------------------

    def _mtime(self):
        try:
            return os.stat(self.cfg_path).st_mtime_ns
        except OSError:
            return None

    def save(self):
        try:
            save_config(self.cfg_path, self.cfg)
        except OSError as e:
            print(f"{APP}: cannot save settings: {e}", file=sys.stderr)
        self.cfg_mtime = self._mtime()

    def set_cfg(self, **kw):
        old = dict(self.cfg)
        self.cfg.update(kw)
        self.save()
        self.apply(old)

    def apply(self, old):
        c = self.cfg
        changed = {k for k in c if c[k] != old.get(k)}
        if changed & {"drives", "include_network"}:
            self.sync_volumes()
        if "font_size" in changed:
            self.set_fonts()
        if "opacity" in changed:
            self.apply_window_attrs()
        if "interval_s" in changed and self.tick_job:
            self.root.after_cancel(self.tick_job)
            self.tick()
        if changed & {"x", "y"} and (c["x"], c["y"]) != self.pos:
            self.place(c["x"], c["y"])
        if changed - {"x", "y"}:
            self.evaluate()
            self.relayout()

    def sync_volumes(self):
        old = {norm_key(v.path): v for v in self.vols}
        vols = []
        for d in wanted_drives(self.cfg):
            v = old.get(norm_key(d.path)) or Vol(d)
            v.name, v.kind, v.fast = d.name, d.kind, d.fast
            vols.append(v)
        changed = [v.path for v in vols] != [v.path for v in self.vols]
        self.vols = vols
        return changed

    def housekeeping(self):
        """Every ~5 s: stay on top, pick up settings-file edits and (un)plugged drives."""
        if not self.busy():
            self.raise_top()
        m = self._mtime()
        if m is not None and m != self.cfg_mtime:
            self.cfg_mtime = m
            cfg, err = load_config(self.cfg_path)
            self.cfg_error = err
            if err:
                print(f"{APP}: {err} (keeping previous settings)", file=sys.stderr)
            else:
                old, self.cfg = self.cfg, cfg
                self.apply(old)
        sig = mounts_signature()
        if sig != self.mount_sig:
            self.mount_sig = sig
            if self.sync_volumes():
                self.evaluate()
                self.relayout()

    # -- menu ----------------------------------------------------------------

    def _check(self, menu, label, value, callback):
        var = tk.BooleanVar(self.root, value)
        self.mvars.append(var)
        menu.add_checkbutton(label=label, variable=var, command=lambda: callback(var.get()))

    def _choices(self, menu, key, options):
        var = tk.StringVar(self.root, f"{float(self.cfg[key]):g}")
        self.mvars.append(var)
        for label, value in options:
            menu.add_radiobutton(label=label, variable=var, value=f"{float(value):g}",
                                 command=lambda v=value: self.set_cfg(**{key: v}))
        if var.get() not in {f"{float(v):g}" for _, v in options}:
            menu.add_radiobutton(label=f"Custom ({var.get()})", variable=var,
                                 value=var.get(), state="disabled")

    def show_menu(self, e):
        if self.menu is not None:
            self.menu.destroy()
        self.mvars = []
        c = self.cfg
        self.menu = m = tk.Menu(self.root, tearoff=0)
        m.add_command(label=f"{APP} {VERSION}", state="disabled")
        if self.cfg_error:
            m.add_command(label="Settings file has an error - see log", state="disabled")
        m.add_separator()

        dm = tk.Menu(m, tearoff=0)
        self._check(dm, "All local drives (auto)", not c["drives"],
                    lambda on: self.set_cfg(drives=[] if on else [v.path for v in self.shown]))
        self._check(dm, "Include network drives in auto", c["include_network"],
                    lambda on: self.set_cfg(include_network=on))
        dm.add_separator()
        shown = {norm_key(v.path) for v in self.shown}
        listed = set()
        for d in discover(True) + [Drive(p, short_path(p), "folder", False) for p in c["drives"]]:
            k = norm_key(d.path)
            if k not in listed:
                listed.add(k)
                self._check(dm, f"{d.name}   ({d.kind})", k in shown,
                            lambda on, p=d.path: self.toggle_drive(p, on))
        dm.add_separator()
        dm.add_command(label="Add folder…", command=self.add_folder)
        m.add_cascade(label="Drives", menu=dm)

        am = tk.Menu(m, tearoff=0)
        for key, title, opts in (
                ("warn_gb", "Amber below", [(f"{g:g} GB free", g) for g in (25, 50, 100, 250, 500, 1000)]),
                ("crit_gb", "Red + alarm below", [(f"{g:g} GB free", g) for g in (5, 10, 20, 50, 100, 250)]),
                ("warn_min", "Amber when full within", [("off", 0)] + [(f"{n} min", n) for n in (30, 60, 120, 240)]),
                ("crit_min", "Red + alarm when full within", [("off", 0)] + [(f"{n} min", n) for n in (5, 10, 15, 30, 60)])):
            sub = tk.Menu(am, tearoff=0)
            self._choices(sub, key, opts)
            am.add_cascade(label=title, menu=sub)
        am.add_separator()
        self._check(am, "Sound alarm", c["sound"], lambda on: self.set_cfg(sound=on))
        m.add_cascade(label="Alerts", menu=am)

        for key, title, opts in (
                ("font_size", "Size", [("Small", 9), ("Medium", 11), ("Large", 14), ("Huge", 20)]),
                ("opacity", "Opacity", [(f"{p}%", p / 100) for p in (100, 90, 80, 70, 50)]),
                ("interval_s", "Update every", [(f"{n} s", n) for n in (1, 2, 5, 10)])):
            sub = tk.Menu(m, tearoff=0)
            self._choices(sub, key, opts)
            m.add_cascade(label=title, menu=sub)
        self._check(m, "Show write rate and time left", c["show_rate"],
                    lambda on: self.set_cfg(show_rate=on))
        m.add_separator()
        if IS_WIN or IS_LINUX:
            self._check(m, "Start at login", autostart_enabled(), self.toggle_autostart)
        m.add_command(label="Edit settings file…", command=self.edit_settings)
        m.add_command(label="Move to corner", command=lambda: (self.place(), self.set_cfg(
            x=self.pos[0], y=self.pos[1])))
        m.add_separator()
        m.add_command(label="Exit", command=self.quit)
        try:
            m.tk_popup(e.x_root, e.y_root)
        finally:
            m.grab_release()

    def toggle_drive(self, path, on):
        cur = [v.path for v in self.shown]
        k = norm_key(path)
        if on and all(norm_key(p) != k for p in cur):
            cur.append(path)
        if not on:
            cur = [p for p in cur if norm_key(p) != k]
        self.set_cfg(drives=cur)            # [] falls back to auto

    def add_folder(self):
        self.modal = True
        try:
            path = filedialog.askdirectory(parent=self.root, mustexist=True,
                                           title="Folder to monitor (its drive's free space)")
        finally:
            self.modal = False
        if path:
            path = os.path.normpath(path)
            cur = self.cfg["drives"] or [v.path for v in self.shown]
            if all(norm_key(p) != norm_key(path) for p in cur):
                self.set_cfg(drives=cur + [path])

    def launch_argv(self):
        if getattr(sys, "frozen", False):
            argv = [sys.executable]
        else:
            exe = sys.executable
            if IS_WIN:
                w = os.path.join(os.path.dirname(exe), "pythonw.exe")
                exe = w if os.path.exists(w) else exe
            argv = [exe, os.path.abspath(sys.argv[0])]
        if os.path.abspath(self.cfg_path) != os.path.abspath(default_config_path()):
            argv += ["--config", os.path.abspath(self.cfg_path)]
        return argv

    def toggle_autostart(self, on):
        try:
            set_autostart(on, self.launch_argv())
        except OSError as e:
            print(f"{APP}: cannot change start-at-login: {e}", file=sys.stderr)

    def edit_settings(self):
        self.save()
        p = self.cfg_path
        try:
            if IS_WIN:
                subprocess.Popen(["notepad.exe", p])
            elif IS_MAC:
                subprocess.Popen(["open", "-t", p])
            else:
                subprocess.Popen(["xdg-open", p], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True)
        except OSError as e:
            print(f"{APP}: cannot open {p}: {e}", file=sys.stderr)

    # -- lifecycle -----------------------------------------------------------

    def on_error(self, *exc):
        traceback.print_exception(*exc)

    def quit(self):
        try:
            self.root.destroy()
        except tk.TclError:
            pass
        for stream in (sys.stdout, sys.stderr):
            try:
                stream.flush()
            except Exception:  # noqa: BLE001
                pass
        os._exit(0)    # don't wait for a probe stuck on a dead network share

    def run(self):
        self.root.mainloop()


# -------------------------------------------------------------------- main --

def single_instance(cfg_path):
    """Hold a per-settings-file lock; None if another Headroom already has it."""
    key = os.path.abspath(cfg_path)
    tag = hashlib.sha1((key.lower() if IS_WIN else key).encode()).hexdigest()[:12]
    if IS_WIN:
        handle = _k32.CreateMutexW(None, False, f"Local\\{APP}-{tag}")
        return None if ctypes.get_last_error() == 183 else handle   # ERROR_ALREADY_EXISTS
    import fcntl
    fd = os.open(os.path.join(tempfile.gettempdir(), f"headroom-{os.getuid()}-{tag}.lock"),
                 os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        return None
    return fd


def lower_priority():
    """Yield the CPU to acquisition software: below-normal priority + efficiency mode."""
    try:
        if IS_WIN:
            proc = _k32.GetCurrentProcess()
            _k32.SetPriorityClass(proc, 0x4000)                       # BELOW_NORMAL
            info = (ctypes.c_ulong * 3)(1, 1, 1)    # PROCESS_POWER_THROTTLING_STATE: EcoQoS on
            _k32.SetProcessInformation(proc, 4, ctypes.byref(info), ctypes.sizeof(info))
            low = ctypes.c_ulong(2)                                    # MEMORY_PRIORITY_LOW
            _k32.SetProcessInformation(proc, 0, ctypes.byref(low), ctypes.sizeof(low))
        elif hasattr(os, "SCHED_IDLE"):             # Linux: run only when a CPU is idle
            os.sched_setscheduler(0, os.SCHED_IDLE, os.sched_param(0))
        else:
            os.nice(10)
    except Exception:  # noqa: BLE001 - older OS versions lack some of these calls
        pass


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Always-on-top free disk space monitor. Right-click the widget for options.")
    ap.add_argument("--config", metavar="FILE", help="settings file (default: headroom.json "
                    "next to this script if present, else the per-user settings folder)")
    ap.add_argument("--drives", nargs="+", metavar="PATH",
                    help="drives/folders to show, e.g. C: D:  or  / /data   ('all' = auto)")
    ap.add_argument("--reset-position", action="store_true", help="move back to the corner")
    ap.add_argument("--version", action="version", version=f"{APP} {VERSION}")
    a = ap.parse_args(argv)

    cfg_path = os.path.abspath(a.config or default_config_path())
    if sys.stderr is None:                     # pythonw: keep a small log instead
        try:
            os.makedirs(os.path.dirname(cfg_path), exist_ok=True)
            sys.stdout = sys.stderr = open(os.path.splitext(cfg_path)[0] + ".log", "w",
                                           buffering=1, encoding="utf-8")
        except OSError:
            sys.stdout = sys.stderr = open(os.devnull, "w")
    lock = single_instance(cfg_path)
    if lock is None:
        print(f"{APP} is already running with {cfg_path}", file=sys.stderr)
        return 0
    cfg, err = load_config(cfg_path)
    if err:
        print(f"{APP}: {err} (using defaults)", file=sys.stderr)
    if a.drives is not None:
        cfg["drives"] = [] if [d.lower() for d in a.drives] == ["all"] else a.drives
    if a.reset_position:
        cfg["x"] = cfg["y"] = None
    if IS_WIN:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)   # crisp text on HiDPI screens
        except Exception:  # noqa: BLE001
            try:
                ctypes.windll.user32.SetProcessDPIAware()
            except Exception:  # noqa: BLE001
                pass
    lower_priority()
    app = App(cfg_path, cfg, err)
    if a.drives is not None or a.reset_position or not os.path.exists(cfg_path):
        app.set_cfg(x=app.pos[0], y=app.pos[1])
    app.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
