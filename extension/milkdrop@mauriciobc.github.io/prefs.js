import Adw from 'gi://Adw';
import Gdk from 'gi://Gdk';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';
import Pango from 'gi://Pango';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';
import {queryMilkdropStatus, queryAllMilkdropStatus} from './controlClient.js';

export default class MilkdropPreferences extends ExtensionPreferences {
    /**
     * Fill the preferences window with settings UI.
     * Declared async for GNOME 47+ compatibility where fillPreferencesWindow is awaited.
     * @param {Adw.PreferencesWindow} window
     */
    async fillPreferencesWindow(window) {
        const settings = this.getSettings();

        const page = new Adw.PreferencesPage({
            title: 'Milkdrop',
            icon_name: 'applications-multimedia-symbolic',
        });
        window.add(page);

        // ── Status ───────────────────────────────────────────────────────────
        // Read-only live view, polled every 2 s while the window is mapped.
        const statusGroup = new Adw.PreferencesGroup({
            title: 'Status',
            description: 'Live renderer information — updated every 2 seconds',
        });
        page.add(statusGroup);

        // Adaptive layout for narrow windows
        const breakpoint = new Adw.Breakpoint({
            condition: Adw.BreakpointCondition.parse('max-width: 450px'),
        });
        breakpoint.add_setter(statusGroup, 'description', '');
        window.add_breakpoint(breakpoint);

        const makeStatusRow = (title, initial) => {
            const row = new Adw.ActionRow({title, activatable: false});
            const label = new Gtk.Label({
                label: initial,
                xalign: 1.0,
                hexpand: true,
                ellipsize: Pango.EllipsizeMode.MIDDLE,
            });
            label.add_css_class('dim-label');
            row.add_suffix(label);
            statusGroup.add(row);
            return label;
        };

        const fpsStatusLabel      = makeStatusRow('FPS', '—');
        const presetStatusLabel   = makeStatusRow('Current Preset', '—');
        const audioStatusLabel    = makeStatusRow('Audio', '—');
        const pausedStatusLabel   = makeStatusRow('Paused', '—');
        const quarantineLabel     = makeStatusRow('Quarantined Presets', '—');

        let pollSourceId = 0;

        const refreshStatus = () => {
            const display = Gdk.Display.get_default();
            const numMonitors = display ? display.get_monitors().get_n_items() : 1;
            queryAllMilkdropStatus(numMonitors).then(results => {
                const active = results.filter(r => r.status !== null);
                if (active.length === 0) {
                    fpsStatusLabel.set_label('—');
                    presetStatusLabel.set_label('—');
                    audioStatusLabel.set_label('—');
                    pausedStatusLabel.set_label('—');
                    quarantineLabel.set_label('—');
                    return;
                }

                // Show aggregate: FPS from first active, combined info for multiple monitors.
                const first = active[0].status;
                if (active.length === 1) {
                    fpsStatusLabel.set_label(
                        Number.isFinite(first.fps) && first.fps > 0
                            ? first.fps.toFixed(1)
                            : '—'
                    );
                    presetStatusLabel.set_label(first.preset || '(none)');
                    audioStatusLabel.set_label(first.audio ?? '—');
                    pausedStatusLabel.set_label(first.paused ? 'Yes' : 'No');
                    quarantineLabel.set_label(String(first.quarantine ?? 0));
                } else {
                    const fpsValues = active.map(r => r.status.fps).filter(f => Number.isFinite(f) && f > 0);
                    fpsStatusLabel.set_label(
                        fpsValues.length > 0
                            ? `${fpsValues.map(f => f.toFixed(1)).join(', ')}`
                            : '—'
                    );
                    const pausedAny = active.some(r => r.status.paused);
                    const audioWorst = active.some(r => r.status.audio === 'failed')
                        ? 'failed'
                        : active.some(r => r.status.audio === 'recovering')
                            ? 'recovering'
                            : 'ok';
                    const totalQuarantine = active.reduce((sum, r) => sum + (r.status.quarantine ?? 0), 0);
                    presetStatusLabel.set_label(`${active.length} monitors active`);
                    audioStatusLabel.set_label(audioWorst);
                    pausedStatusLabel.set_label(pausedAny ? 'Some' : 'No');
                    quarantineLabel.set_label(String(totalQuarantine));
                }
            }).catch(() => {});
        };

        window.connect('map', () => {
            refreshStatus();
            pollSourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 2000, () => {
                refreshStatus();
                return GLib.SOURCE_CONTINUE;
            });
        });
        window.connect('unmap', () => {
            if (pollSourceId > 0) {
                GLib.source_remove(pollSourceId);
                pollSourceId = 0;
            }
        });

        // ── Runtime ──────────────────────────────────────────────────────────
        const runtimeGroup = new Adw.PreferencesGroup({title: 'Runtime'});
        page.add(runtimeGroup);

        const enabledRow = new Adw.SwitchRow({
            title: 'Enabled',
            subtitle: 'Spawn and supervise the renderer process',
        });
        settings.bind('enabled', enabledRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        runtimeGroup.add(enabledRow);

        const monitorRow = new Adw.SpinRow({
            title: 'Monitor Index',
            subtitle: 'Zero-based index of the monitor to render on',
            adjustment: new Gtk.Adjustment({
                value: 0,
                lower: 0,
                upper: 16,
                step_increment: 1,
                page_increment: 1,
            }),
            digits: 0,
        });
        settings.bind('monitor', monitorRow, 'value', Gio.SettingsBindFlags.DEFAULT);
        runtimeGroup.add(monitorRow);

        const allMonitorsRow = new Adw.SwitchRow({
            title: 'All Monitors',
            subtitle: 'Show visualizer on every connected monitor simultaneously',
        });
        settings.bind('all-monitors', allMonitorsRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        runtimeGroup.add(allMonitorsRow);

        // Disable the monitor index row when all-monitors is active
        const syncMonitorSensitivity = () => {
            monitorRow.sensitive = !settings.get_boolean('all-monitors');
        };
        syncMonitorSensitivity();
        const allMonitorsSignalId = settings.connect('changed::all-monitors', syncMonitorSensitivity);

        // Cleanup on window destroy
        window.connect('destroy', () => {
            if (allMonitorsSignalId)
                settings.disconnect(allMonitorsSignalId);
        });

        const opacityRow = new Adw.ActionRow({
            title: 'Opacity',
            subtitle: 'Renderer window transparency',
            activatable: false,
        });
        const opacityScale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 1.0, 0.01);
        opacityScale.set_hexpand(true);
        opacityScale.set_digits(2);
        opacityScale.set_valign(Gtk.Align.CENTER);
        settings.bind('opacity', opacityScale, 'value', Gio.SettingsBindFlags.DEFAULT);
        opacityRow.add_suffix(opacityScale);
        runtimeGroup.add(opacityRow);

        // ── Performance ──────────────────────────────────────────────────────
        const perfGroup = new Adw.PreferencesGroup({title: 'Performance'});
        page.add(perfGroup);

        const fpsRow = new Adw.SpinRow({
            title: 'Frame Rate',
            subtitle: 'Target frames per second (applied live)',
            adjustment: new Gtk.Adjustment({
                value: 60,
                lower: 10,
                upper: 144,
                step_increment: 1,
                page_increment: 10,
            }),
            digits: 0,
        });
        settings.bind('fps', fpsRow, 'value', Gio.SettingsBindFlags.DEFAULT);
        perfGroup.add(fpsRow);

        // ── Behavior ─────────────────────────────────────────────────────────
        const behaviorGroup = new Adw.PreferencesGroup({title: 'Behavior'});
        page.add(behaviorGroup);

        // Preset rotation: Sequential / Shuffle (replaces the old SwitchRow)
        const rotationRow = new Adw.ComboRow({
            title: 'Preset Rotation',
            subtitle: 'Order in which presets are selected',
            model: Gtk.StringList.new(['Sequential', 'Shuffle']),
        });
        rotationRow.selected = settings.get_boolean('shuffle') ? 1 : 0;
        rotationRow.connect('notify::selected', () => {
            settings.set_boolean('shuffle', rotationRow.selected === 1);
        });
        settings.connect('changed::shuffle', () => {
            const expected = settings.get_boolean('shuffle') ? 1 : 0;
            if (rotationRow.selected !== expected)
                rotationRow.selected = expected;
        });
        behaviorGroup.add(rotationRow);

        const rotationIntervalRow = new Adw.SpinRow({
            title: 'Rotation Interval',
            subtitle: 'Seconds before switching to the next preset',
            adjustment: new Gtk.Adjustment({
                value: 30,
                lower: 5,
                upper: 300,
                step_increment: 5,
                page_increment: 30,
            }),
            digits: 0,
        });
        settings.bind('preset-rotation-interval', rotationIntervalRow, 'value', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(rotationIntervalRow);

        const overlayRow = new Adw.SwitchRow({
            title: 'Overlay Mode',
            subtitle: 'Pass --overlay to the renderer; state is visible in the Status section',
        });
        settings.bind('overlay', overlayRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(overlayRow);

        const pauseFsRow = new Adw.SwitchRow({
            title: 'Pause on Fullscreen',
            subtitle: 'Pause when a fullscreen window is on the same monitor',
        });
        settings.bind('pause-on-fullscreen', pauseFsRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(pauseFsRow);

        const pauseMaxRow = new Adw.SwitchRow({
            title: 'Pause when Maximized',
            subtitle: 'Pause when a maximized window is on the same monitor',
        });
        settings.bind('pause-on-maximized', pauseMaxRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(pauseMaxRow);

        const mediaAwareRow = new Adw.SwitchRow({
            title: 'Media-Aware Mode',
            subtitle: 'Pause when no MPRIS media player is playing',
        });
        settings.bind('media-aware', mediaAwareRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(mediaAwareRow);

        // ── Presets ──────────────────────────────────────────────────────────
        const presetGroup = new Adw.PreferencesGroup({title: 'Presets'});
        page.add(presetGroup);

        const dirRow = new Adw.ActionRow({
            title: 'Directory',
            subtitle: 'Folder containing .milk preset files',
            activatable: false,
        });

        const dirLabel = new Gtk.Label({
            label: settings.get_string('preset-dir') || '(default)',
            xalign: 0,
            hexpand: true,
            ellipsize: Pango.EllipsizeMode.START,
            valign: Gtk.Align.CENTER,
        });
        dirLabel.add_css_class('dim-label');

        settings.connect('changed::preset-dir', () => {
            dirLabel.set_label(settings.get_string('preset-dir') || '(default)');
        });

        const browseBtn = new Gtk.Button({
            label: 'Browse…',
            valign: Gtk.Align.CENTER,
        });
        browseBtn.add_css_class('flat');
        browseBtn.connect('clicked', () => {
            const dialog = new Gtk.FileDialog({title: 'Select Preset Directory'});
            const initialPath = settings.get_string('preset-dir');
            if (initialPath) {
                try {
                    dialog.initial_folder = Gio.File.new_for_path(initialPath);
                } catch (_e) { /* ignore */ }
            }
            dialog.select_folder(window, null, (_dialog, res) => {
                try {
                    const file = _dialog.select_folder_finish(res);
                    if (file)
                        settings.set_string('preset-dir', file.get_path());
                } catch (_e) {
                    // User cancelled — no action needed
                }
            });
        });

        dirRow.add_suffix(dirLabel);
        dirRow.add_suffix(browseBtn);
        presetGroup.add(dirRow);
    }
}
