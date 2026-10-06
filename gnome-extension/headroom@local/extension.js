// Headroom for the GNOME top bar: free space, write rate and time-until-full of
// every (or selected) drive, turning amber / flashing red with a notification
// and sound before a recording can fill a disk.
//
// Runs inside the shell (no extra process).  Disk queries are asynchronous (GIO
// worker threads, one statvfs per drive per interval, no file scanning), so a
// hung network share can never freeze the desktop.  Keeps running on the lock
// screen so the alarm still sounds.  Settings: ~/.config/headroom/headroom.json
// (shared with headroom.py).

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Clutter from 'gi://Clutter';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import * as Core from './core.js';

const COLORS = {[Core.NA]: '#9da7b3', [Core.OK]: '#3fb950', [Core.WARN]: '#e3b341', [Core.CRIT]: '#ff6b61'};

const esc = s => GLib.markup_escape_text(s, -1);
const now = () => GLib.get_monotonic_time() / 1e6;
const normPath = p => (p.length > 1 ? p.replace(/\/+$/, '') : p);

const Indicator = GObject.registerClass(
class HeadroomIndicator extends PanelMenu.Button {
    _init() {
        super._init(0.5, 'Headroom');
        this._box = new St.BoxLayout({style_class: 'panel-status-menu-box headroom'});
        this._box.add_child(new St.Icon({icon_name: 'drive-harddisk-symbolic', style_class: 'system-status-icon'}));
        this.label = new St.Label({text: '…', y_align: Clutter.ActorAlign.CENTER});
        this.label.clutter_text.use_markup = true;
        this._box.add_child(this.label);
        this.add_child(this._box);
    }

    setFlash(on) {
        if (on)
            this._box.add_style_class_name('headroom-flash');
        else
            this._box.remove_style_class_name('headroom-flash');
    }
});

export default class HeadroomExtension extends Extension {
    enable() {
        this._cfg = Core.parseConfig('{}').cfg;
        this._cfgError = null;
        this._lastWritten = null;
        this._vols = [];
        this._mounts = null;
        this._markup = null;
        this._renderId = this._timerId = this._flashId = 0;
        this._flashOn = false;
        this._critSince = null;
        this._lastBeep = -Infinity;
        this._notified = false;
        this._hkTime = now();
        this._cancellable = new Gio.Cancellable();
        this._cfgFile = Gio.File.new_for_path(
            GLib.build_filenamev([GLib.get_user_config_dir(), 'headroom', 'headroom.json']));

        this._indicator = new Indicator();
        this._indicator.menu.connect('open-state-changed', (_m, open) => {
            if (open)
                this._buildMenu();
        });
        Main.panel.addToStatusArea(this.uuid, this._indicator, 0, 'right');

        // settings edited elsewhere (headroom.py, a text editor): inotify, no polling
        this._fileMonitor = this._cfgFile.monitor_file(Gio.FileMonitorFlags.NONE, null);
        this._fileMonitor.connect('changed', (_m, _f, _o, event) => {
            if (event === Gio.FileMonitorEvent.CHANGES_DONE_HINT || event === Gio.FileMonitorEvent.CREATED)
                this._loadConfig();
        });
        this._loadConfig();
        this._syncVolumes();
        this._buildMenu(); // GNOME won't open an empty menu; it is rebuilt fresh on every open
        this._tick();
        this._restartTimer();
    }

    disable() {
        for (const id of [this._timerId, this._flashId, this._renderId]) {
            if (id)
                GLib.Source.remove(id);
        }
        this._timerId = this._flashId = this._renderId = 0;
        this._cancellable.cancel();
        this._fileMonitor.cancel();
        this._fileMonitor = null;
        this._indicator.destroy();
        this._indicator = null;
        this._vols = [];
    }

    // ------------------------------------------------------------ settings

    _loadConfig() {
        this._cfgFile.load_contents_async(this._cancellable, (file, res) => {
            if (!this._indicator)
                return;
            let text;
            try {
                text = new TextDecoder().decode(file.load_contents_finish(res)[1]);
            } catch (e) {
                if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND))
                    this._saveConfig(); // first run: write the defaults so they can be edited
                else if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                    this._cfgError = e.message;
                return;
            }
            if (text === this._lastWritten)
                return; // our own write
            const {cfg, error} = Core.parseConfig(text);
            this._cfgError = error;
            if (error)
                console.warn(`Headroom: ${error} (keeping previous settings)`);
            else
                this._applyConfig(cfg);
        });
    }

    _saveConfig() {
        const text = Core.serializeConfig(this._cfg);
        this._lastWritten = text;
        this._cfgFile.get_parent().make_directory_async(GLib.PRIORITY_DEFAULT, this._cancellable, (dir, res) => {
            try {
                dir.make_directory_finish(res);
            } catch (e) {} // already exists
            this._cfgFile.replace_contents_bytes_async(new GLib.Bytes(new TextEncoder().encode(text)), null,
                false, Gio.FileCreateFlags.NONE, this._cancellable, (f, r) => {
                    try {
                        f.replace_contents_finish(r);
                    } catch (e) {
                        if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                            console.warn(`Headroom: cannot save settings: ${e.message}`);
                    }
                });
        });
    }

    _applyConfig(cfg) {
        const old = this._cfg;
        this._cfg = cfg;
        if (JSON.stringify(old.drives) !== JSON.stringify(cfg.drives) || old.include_network !== cfg.include_network)
            this._syncVolumes();
        if (old.interval_s !== cfg.interval_s)
            this._restartTimer();
        this._update();
    }

    _setCfg(changes) {
        this._applyConfig({...this._cfg, ...changes});
        this._saveConfig();
    }

    // ------------------------------------------------------------ measuring

    _restartTimer() {
        if (this._timerId)
            GLib.Source.remove(this._timerId);
        const s = this._cfg.interval_s;
        const tick = () => {
            try {
                this._tick();
            } catch (e) {
                console.error(e); // never let an error stop the monitoring
            }
            return GLib.SOURCE_CONTINUE;
        };
        // whole seconds let GLib batch our wakeup with other timers
        this._timerId = Number.isInteger(s) ? GLib.timeout_add_seconds(GLib.PRIORITY_LOW, s, tick)
            : GLib.timeout_add(GLib.PRIORITY_LOW, Math.round(s * 1000), tick);
    }

    _mountsText() {
        try {
            return new TextDecoder().decode(GLib.file_get_contents('/proc/self/mounts')[1]);
        } catch (e) {
            return '';
        }
    }

    _syncVolumes() {
        const c = this._cfg;
        this._mounts = this._mountsText();
        let want;
        if (!c.drives.length) {
            want = Core.parseMounts(this._mounts, c.include_network);
        } else {
            const known = new Map(Core.parseMounts(this._mounts, true).map(d => [normPath(d.path), d]));
            want = c.drives.map(p => known.get(normPath(p)) ?? {path: p, name: Core.shortPath(p), kind: 'folder'});
        }
        const old = new Map(this._vols.map(v => [v.path, v]));
        this._vols = want.map(d => old.get(d.path) ?? new Core.Vol(d.path, d.name, d.kind));
    }

    _tick() {
        const t = now();
        for (const v of this._vols)
            this._probe(v);
        if (t - this._hkTime >= 10) { // drives mounted / unmounted?
            this._hkTime = t;
            if (this._mountsText() !== this._mounts)
                this._syncVolumes();
        }
        this._update(); // also shows "no response" for a hung share
    }

    _probe(v) {
        if (v.inflight)
            return; // a hung network share: don't pile up queries
        v.inflight = true;
        v.sent = now();
        Gio.File.new_for_path(v.path).query_filesystem_info_async('filesystem::size,filesystem::free',
            GLib.PRIORITY_LOW, this._cancellable, (file, res) => {
                v.inflight = false;
                if (!this._indicator)
                    return;
                try {
                    const info = file.query_filesystem_info_finish(res);
                    v.store(now(), info.get_attribute_uint64('filesystem::size'),
                        info.get_attribute_uint64('filesystem::free'), this._cfg.rate_window_s);
                } catch (e) {
                    if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        return;
                    v.fail(e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND) ? 'not found'
                        : e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.PERMISSION_DENIED) ? 'no access' : 'unavailable');
                }
                if (!this._renderId) { // results of one tick arrive together: draw once
                    this._renderId = GLib.idle_add(GLib.PRIORITY_LOW, () => {
                        this._renderId = 0;
                        this._update();
                        return GLib.SOURCE_REMOVE;
                    });
                }
            });
    }

    // ------------------------------------------------------------ alerts + drawing

    _update() {
        if (!this._indicator)
            return;
        this._evaluate();
        this._render();
    }

    _evaluate() {
        const c = this._cfg, t = now();
        for (const v of this._vols)
            v.level = v.err ? Core.NA : Core.levelOf(v.free, v.total, v.rate, c);
        const crit = this._vols.filter(v => v.level === Core.CRIT);
        if (!crit.length) {
            this._critSince = null;
            this._lastBeep = -Infinity;
            this._notified = false;
            if (this._flashId) {
                GLib.Source.remove(this._flashId);
                this._flashId = 0;
                this._indicator.setFlash(false);
            }
            return;
        }
        this._critSince ??= t;
        if (t - this._critSince >= Math.max(3, 1.5 * c.interval_s)) { // ignore one-sample blips
            if (!this._notified) {
                this._notified = true;
                const v = crit[0], eta = v.eta;
                Main.notify(`Disk almost full: ${v.name}`, `${Core.fmtSize(v.free)} free` +
                    (eta !== null ? `, full in ${Core.fmtEta(eta)} at ${Core.fmtRate(v.rate)}` : ''));
            }
            if (c.sound && t - this._lastBeep >= c.alarm_repeat_s) {
                this._lastBeep = t;
                global.display.get_sound_player().play_from_theme('alarm-clock-elapsed', 'Disk almost full', null);
            }
        }
        if (!this._flashId) { // flash timer only runs while something is red
            this._flashId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {
                this._flashOn = !this._flashOn;
                this._indicator.setFlash(this._flashOn);
                return GLib.SOURCE_CONTINUE;
            });
        }
    }

    _render() {
        const c = this._cfg, t = now(), parts = [];
        for (const v of this._vols) {
            let s = `${esc(v.name)} `;
            const stale = v.inflight && t - v.sent > Math.max(3 * c.interval_s, 6);
            if (v.err) {
                s += `<span foreground="${COLORS[Core.NA]}">${esc(v.err)}</span>`;
            } else if (!(v.total > 0)) {
                s += '…';
            } else {
                s += `<b><span foreground="${stale ? COLORS[Core.NA] : COLORS[v.level]}">${esc(Core.fmtSize(v.free))}</span></b>`;
                const eta = v.eta;
                if (c.show_rate && eta !== null && !stale) {
                    const el = Core.etaLevel(eta, c);
                    s += el > Core.OK ? ` <span foreground="${COLORS[el]}">${esc(Core.fmtEta(eta))}</span>`
                        : ` ${esc(Core.fmtEta(eta))}`;
                }
            }
            parts.push(s);
        }
        const markup = parts.length ? parts.join('    ') : 'no drives';
        if (markup !== this._markup) { // only touch the top bar when something changed
            this._markup = markup;
            this._indicator.label.clutter_text.set_markup(markup);
        }
    }

    // ------------------------------------------------------------ menu

    _detail(v) {
        if (v.err)
            return `${v.name} — ${v.err}`;
        if (!(v.total > 0))
            return `${v.name} — measuring…`;
        let s = `${v.name} — ${Core.fmtSize(v.free)} free of ${Core.fmtSize(v.total)} (${Math.round(100 * v.free / v.total)}%)`;
        if (v.eta !== null)
            s += `, writing ${Core.fmtRate(v.rate)}, full in ${Core.fmtEta(v.eta)}`;
        return s;
    }

    _switch(menu, label, value, onToggle) {
        const item = new PopupMenu.PopupSwitchMenuItem(label, value);
        item.connect('toggled', (_i, state) => onToggle(state));
        menu.addMenuItem(item);
    }

    /** one row of preset buttons; the menu stays open while choosing */
    _chooser(menu, title, key, options) {
        const item = new PopupMenu.PopupBaseMenuItem({reactive: false, can_focus: false});
        const row = new St.BoxLayout({x_expand: true, style_class: 'headroom-row'});
        row.add_child(new St.Label({text: title, x_expand: true, y_align: Clutter.ActorAlign.CENTER}));
        const buttons = options.map(([label, value]) => {
            const b = new St.Button({label, style_class: 'headroom-choice', can_focus: true, toggle_mode: true,
                checked: Math.abs(this._cfg[key] - value) < 1e-9});
            b.connect('clicked', () => {
                buttons.forEach(o => (o.checked = o === b));
                this._setCfg({[key]: value});
            });
            row.add_child(b);
            return b;
        });
        item.add_child(row);
        menu.addMenuItem(item);
    }

    _buildMenu() {
        const menu = this._indicator.menu, c = this._cfg;
        menu.removeAll();
        for (const v of this._vols)
            menu.addMenuItem(new PopupMenu.PopupMenuItem(this._detail(v), {reactive: false}));
        if (!this._vols.length)
            menu.addMenuItem(new PopupMenu.PopupMenuItem('No drives', {reactive: false}));
        if (Main.sessionMode.isLocked)
            return; // lock screen: read-only
        if (this._cfgError)
            menu.addMenuItem(new PopupMenu.PopupMenuItem(`Settings file error: ${this._cfgError}`, {reactive: false}));
        menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        const drives = new PopupMenu.PopupSubMenuMenuItem('Drives');
        this._switch(drives.menu, 'All local drives (auto)', !c.drives.length,
            on => this._setCfg({drives: on ? [] : this._vols.map(v => v.path)}));
        this._switch(drives.menu, 'Include network drives in auto', c.include_network,
            on => this._setCfg({include_network: on}));
        drives.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        const shown = new Set(this._vols.map(v => normPath(v.path))), listed = new Set();
        const candidates = [...Core.parseMounts(this._mountsText(), true),
            ...c.drives.map(p => ({path: p, name: Core.shortPath(p), kind: 'folder'}))];
        for (const d of candidates) {
            const k = normPath(d.path);
            if (listed.has(k))
                continue;
            listed.add(k);
            this._switch(drives.menu, `${d.name}  (${d.kind})`, shown.has(k), on => {
                const cur = this._vols.map(v => v.path).filter(p => normPath(p) !== k);
                this._setCfg({drives: on ? [...cur, d.path] : cur});
            });
        }
        menu.addMenuItem(drives);

        const alerts = new PopupMenu.PopupSubMenuMenuItem('Alerts');
        this._chooser(alerts.menu, 'Amber below (GB free)', 'warn_gb', [25, 50, 100, 250, 500].map(g => [`${g}`, g]));
        this._chooser(alerts.menu, 'Red + alarm below (GB free)', 'crit_gb', [5, 10, 20, 50, 100].map(g => [`${g}`, g]));
        this._chooser(alerts.menu, 'Amber when full within', 'warn_min',
            [['off', 0], ['30m', 30], ['1h', 60], ['2h', 120], ['4h', 240]]);
        this._chooser(alerts.menu, 'Red + alarm when full within', 'crit_min',
            [['off', 0], ['5m', 5], ['10m', 10], ['15m', 15], ['30m', 30]]);
        this._chooser(alerts.menu, 'Update every', 'interval_s', [1, 2, 5, 10].map(s => [`${s}s`, s]));
        this._switch(alerts.menu, 'Sound alarm', c.sound, on => this._setCfg({sound: on}));
        menu.addMenuItem(alerts);

        this._switch(menu, 'Show time left while writing', c.show_rate, on => this._setCfg({show_rate: on}));
        const edit = new PopupMenu.PopupMenuItem('Edit settings file…');
        edit.connect('activate', () => {
            const app = Gio.AppInfo.get_default_for_type('text/plain', false);
            const ctx = global.create_app_launch_context(0, -1);
            try {
                if (app)
                    app.launch([this._cfgFile], ctx);
                else
                    Gio.AppInfo.launch_default_for_uri(this._cfgFile.get_uri(), ctx);
            } catch (e) {
                Main.notifyError('Headroom', `Cannot open ${this._cfgFile.get_path()}: ${e.message}`);
            }
        });
        menu.addMenuItem(edit);
    }
}
