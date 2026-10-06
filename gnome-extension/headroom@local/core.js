// Platform-independent part of the Headroom GNOME extension (no Shell imports,
// so it can be unit-tested with plain gjs).  Same behaviour as headroom.py.

export const MiB = 2 ** 20, GiB = 2 ** 30, TiB = 2 ** 40, PiB = 2 ** 50;
export const IDLE_RATE = 256 * 1024; // bytes/s; a slower net change counts as "idle"
export const NA = -1, OK = 0, WARN = 1, CRIT = 2;

export const DEFAULTS = Object.freeze({
    drives: [],              // [] = every local drive (auto); or e.g. ["/", "/data"]
    include_network: false,  // auto mode: also show network shares
    interval_s: 2,
    warn_gb: 100,
    crit_gb: 20,
    warn_min: 60,            // 0 = off
    crit_min: 15,            // 0 = off
    rate_window_s: 30,
    show_rate: true,
    sound: true,
    alarm_repeat_s: 30,
    font_size: 11,
    opacity: 0.92,
    x: null,
    y: null,
});

const LIMITS = {
    interval_s: [0.5, 3600], warn_gb: [0, 1e9], crit_gb: [0, 1e9], warn_min: [0, 1e6],
    crit_min: [0, 1e6], rate_window_s: [5, 3600], alarm_repeat_s: [2, 86400],
    font_size: [6, 72], opacity: [0.2, 1],
};

/** Lenient parse: unknown keys are kept (other Headroom versions use them), bad values fall back. */
export function parseConfig(text) {
    const cfg = {...DEFAULTS, drives: []};
    let data;
    try {
        data = JSON.parse(text);
    } catch (e) {
        return {cfg, error: `headroom.json: ${e.message}`};
    }
    if (data === null || typeof data !== 'object' || Array.isArray(data))
        return {cfg, error: 'headroom.json: expected a JSON object'};
    for (const [key, v] of Object.entries(data)) {
        const dflt = DEFAULTS[key];
        const num = typeof v === 'number' && Number.isFinite(v);
        if (!(key in DEFAULTS)) {
            cfg[key] = v;
        } else if (key === 'drives') {
            const list = typeof v === 'string' ? [v] : v;
            if (Array.isArray(list) && list.every(s => typeof s === 'string'))
                cfg.drives = list.map(s => s.trim()).filter(s => s);
        } else if (key === 'x' || key === 'y') {
            cfg[key] = num ? Math.trunc(v) : null;
        } else if (typeof dflt === 'boolean') {
            if (typeof v === 'boolean')
                cfg[key] = v;
        } else if (num) {
            const [lo, hi] = LIMITS[key];
            cfg[key] = Math.min(Math.max(v, lo), hi);
        }
    }
    return {cfg, error: null};
}

export function serializeConfig(cfg) {
    return `${JSON.stringify(cfg, null, 2)}\n`;
}

function fixed(v, unit) {
    const d = v < 9.995 ? 2 : v < 99.95 ? 1 : 0;
    return `${v.toFixed(d)} ${unit}`;
}

export function fmtSize(n) {
    let v = n / MiB, unit = 'MB';
    for (const [div, u] of [[GiB, 'GB'], [TiB, 'TB'], [PiB, 'PB']]) {
        if (v < 999.5)
            break;
        v = n / div;
        unit = u;
    }
    return fixed(v, unit);
}

export function fmtRate(r) {
    let v = r / MiB, unit = 'MB/s';
    if (v >= 999.5) {
        v = r / GiB;
        unit = 'GB/s';
    }
    return `${v.toFixed(v < 99.95 ? 1 : 0)} ${unit}`;
}

export function fmtEta(seconds) {
    let s = Math.trunc(seconds);
    if (s < 60)
        return `${s}s`;
    const m = Math.trunc(s / 60);
    s %= 60;
    if (m < 10)
        return `${m}m${String(s).padStart(2, '0')}s`;
    if (m < 60)
        return `${m}m`;
    const h = Math.trunc(m / 60);
    if (h < 48)
        return `${h}h${String(m % 60).padStart(2, '0')}m`;
    const d = Math.trunc(h / 24);
    return d < 100 ? `${d}d${h % 24}h` : '>99d';
}

export function shortPath(p, n = 22) {
    if ([...p].length <= n)
        return p;
    const parts = p.split(/[\\/]/).filter(s => s);
    const s = [...`…/${parts.slice(-2).join('/')}`];
    return s.length <= n ? s.join('') : `…${s.slice(-(n - 1)).join('')}`;
}

function slope(pts) {
    const n = pts.length;
    const mt = pts.reduce((a, p) => a + p[0], 0) / n;
    const mf = pts.reduce((a, p) => a + p[1], 0) / n;
    let sxx = 0, sxy = 0;
    for (const [t, f] of pts) {
        sxx += (t - mt) ** 2;
        sxy += (t - mt) * (f - mf);
    }
    return sxx > 0 ? sxy / sxx : null;
}

/** Sustained consumption in bytes/s: median of the least-squares trends of the
 *  window's thirds, so a recording shows its true rate while a one-off file
 *  copy doesn't look like one.  hist = [[t, free], ...] */
export function writeRate(hist) {
    const n = hist.length;
    if (n < 5 || hist[n - 1][0] - hist[0][0] < 3)
        return null;
    const i = Math.trunc(n / 3), j = Math.trunc(2 * n / 3);
    const s = [slope(hist.slice(0, i + 1)), slope(hist.slice(i, j + 1)), slope(hist.slice(j))];
    if (s.includes(null))
        return null;
    s.sort((a, b) => a - b);
    return -s[1];
}

export function etaLevel(seconds, cfg) {
    const m = seconds / 60;
    return m < cfg.crit_min ? CRIT : m < cfg.warn_min ? WARN : OK;
}

export function levelOf(free, total, rate, cfg) {
    if (!(total > 0))
        return NA;
    const gb = free / GiB;
    let lvl = gb < cfg.crit_gb ? CRIT : gb < cfg.warn_gb ? WARN : OK;
    if (rate !== null && rate > IDLE_RATE)
        lvl = Math.max(lvl, etaLevel(free / rate, cfg));
    return lvl;
}

const FAST_FS = new Set(['ext2', 'ext3', 'ext4', 'xfs', 'btrfs', 'zfs', 'f2fs', 'jfs', 'reiserfs',
    'bcachefs', 'nilfs2', 'ntfs3', 'exfat', 'vfat', 'msdos', 'hfs', 'hfsplus', 'apfs', 'udf']);
const LOCAL_FS = new Set([...FAST_FS, 'fuseblk', 'ntfs']);
const NET_FS = new Set(['nfs', 'nfs4', 'cifs', 'smb3', 'smbfs', '9p', 'ceph', 'glusterfs', 'lustre',
    'beegfs', 'gpfs', 'fuse.sshfs', 'fuse.rclone', 'fuse.glusterfs', 'fuse.s3fs']);
const SKIP = ['/proc', '/sys', '/dev', '/run', '/boot', '/efi', '/snap', '/var/snap',
    '/var/lib/docker', '/var/lib/containers', '/var/lib/kubelet'];

const unoct = s => s.replace(/\\([0-7]{3})/g, (_, o) => String.fromCharCode(parseInt(o, 8)));

/** Real filesystems from /proc/self/mounts: [{path, name, kind}] (kind: local | network). */
export function parseMounts(text, includeNetwork) {
    const out = [], seen = new Set();
    for (const line of text.split('\n')) {
        const parts = line.split(' ');
        if (parts.length < 3)
            continue;
        const [src, mp, fs] = [unoct(parts[0]), unoct(parts[1]), parts[2]];
        const kind = LOCAL_FS.has(fs) ? 'local' : NET_FS.has(fs) ? 'network' : null;
        if (!kind || seen.has(src))
            continue;
        if (mp !== '/' && !mp.startsWith('/run/media/') &&
            SKIP.some(p => mp === p || mp.startsWith(`${p}/`)))
            continue;
        seen.add(src);
        if (includeNetwork || kind !== 'network')
            out.push({path: mp, name: shortPath(mp), kind});
    }
    return out;
}

/** A monitored volume and its free-space history. */
export class Vol {
    constructor(path, name, kind) {
        this.path = path;
        this.name = name;
        this.kind = kind;
        this.total = 0;
        this.free = 0;
        this.err = null;
        this.hist = [];
        this.rate = null;
        this.level = NA;
        this.inflight = false;
        this.sent = 0;
    }

    store(t, total, free, window) {
        this.err = null;
        this.total = total;
        this.free = free;
        this.hist.push([t, free]);
        while (t - this.hist[0][0] > window)
            this.hist.shift();
        this.rate = writeRate(this.hist);
    }

    fail(msg) {
        this.err = msg;
        this.rate = null;
        this.hist = [];
    }

    get eta() {
        return this.rate !== null && this.rate > IDLE_RATE ? this.free / this.rate : null;
    }
}
