import Adw from 'gi://Adw';
import Gdk from 'gi://Gdk';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';
import Pango from 'gi://Pango';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';
import {queryAllMilkdropStatus} from './controlClient.js';

export default class MilkdropPreferences extends ExtensionPreferences {
    /**
     * Fill the preferences window with tabbed settings UI.
     * Declared async for GNOME 47+ compatibility where fillPreferencesWindow is awaited.
     * @param {Adw.PreferencesWindow} window
     */
    async fillPreferencesWindow(window) {
        const settings = this.getSettings();

        window.set_title(this.metadata.name);
        window.search_enabled = true;
        window.set_default_size(740, 520);

        this._settings = settings;
        this._signalIds = [];
        this._pollSourceId = 0;

        this._buildGeneralPage(window, settings);
        this._buildRenderingPage(window, settings);
        this._buildBehaviorPage(window, settings);
        this._buildTransitionsPage(window, settings);

        window.connect('destroy', () => {
            this._signalIds.forEach(id => settings.disconnect(id));
            this._signalIds = [];
            if (this._pollSourceId > 0) {
                GLib.source_remove(this._pollSourceId);
                this._pollSourceId = 0;
            }
            if (this._mapHandler)
                window.disconnect(this._mapHandler);
            if (this._unmapHandler)
                window.disconnect(this._unmapHandler);
        });
    }

    _buildGeneralPage(window, settings) {
        const page = new Adw.PreferencesPage({
            title: 'General',
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
            const row = new Adw.ActionRow({
                title,
                activatable: false,
            });
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

        const fpsStatusLabel = makeStatusRow('Frames Per Second', '—');
        const presetStatusLabel = makeStatusRow('Current Preset', '—');
        const audioStatusLabel = makeStatusRow('Audio', '—');
        const pausedStatusLabel = makeStatusRow('Paused', '—');
        const quarantineLabel = makeStatusRow('Quarantined Presets', '—');

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

        this._mapHandler = window.connect('map', () => {
            refreshStatus();
            this._pollSourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 2000, () => {
                refreshStatus();
                return GLib.SOURCE_CONTINUE;
            });
        });
        this._unmapHandler = window.connect('unmap', () => {
            if (this._pollSourceId > 0) {
                GLib.source_remove(this._pollSourceId);
                this._pollSourceId = 0;
            }
        });

        // ── General ──────────────────────────────────────────────────────────
        const generalGroup = new Adw.PreferencesGroup({title: 'General'});
        page.add(generalGroup);

        const enabledRow = new Adw.SwitchRow({
            title: 'Enabled',
            subtitle: 'Start the visualizer renderer',
        });
        settings.bind('enabled', enabledRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        generalGroup.add(enabledRow);

        const monitorRow = new Adw.SpinRow({
            title: 'Monitor',
            subtitle: 'Which monitor to display the visualizer on',
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
        generalGroup.add(monitorRow);

        const allMonitorsRow = new Adw.SwitchRow({
            title: 'All Monitors',
            subtitle: 'Show the visualizer on every connected monitor',
        });
        settings.bind('all-monitors', allMonitorsRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        generalGroup.add(allMonitorsRow);

        // Disable the monitor index row when all-monitors is active
        const syncMonitorSensitivity = () => {
            monitorRow.sensitive = !settings.get_boolean('all-monitors');
        };
        syncMonitorSensitivity();
        const allMonitorsSignalId = settings.connect('changed::all-monitors', syncMonitorSensitivity);
        this._signalIds.push(allMonitorsSignalId);

        const opacityRow = new Adw.ActionRow({
            title: 'Opacity',
            subtitle: 'Window transparency',
        });
        const opacityScale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 1.0, 0.01);
        opacityScale.set_hexpand(true);
        opacityScale.set_digits(2);
        opacityScale.set_valign(Gtk.Align.CENTER);
        opacityScale.set_draw_value(true);
        settings.bind('opacity', opacityScale, 'value', Gio.SettingsBindFlags.DEFAULT);
        opacityRow.add_suffix(opacityScale);
        opacityRow.activatable_widget = opacityScale;
        generalGroup.add(opacityRow);
    }

    _buildRenderingPage(window, settings) {
        const page = new Adw.PreferencesPage({
            title: 'Rendering',
            icon_name: 'applications-graphics-symbolic',
        });
        window.add(page);

        // ── GPU ──────────────────────────────────────────────────────────────
        const gpuGroup = new Adw.PreferencesGroup({
            title: 'GPU',
            description: 'Select which graphics card renders the visualizer',
        });
        page.add(gpuGroup);

        const gpuProfileRow = new Adw.ComboRow({
            title: 'Rendering Profile',
            subtitle: 'Graphics card for hybrid graphics systems',
            model: Gtk.StringList.new(['System Default', 'Discrete GPU (DRI PRIME)', 'NVIDIA Optimus']),
        });
        const gpuProfileMap = ['default', 'dri-prime', 'nvidia-optimus'];
        const initialGpuProfile = settings.get_string('gpu-profile');
        gpuProfileRow.selected = gpuProfileMap.indexOf(initialGpuProfile) >= 0
            ? gpuProfileMap.indexOf(initialGpuProfile)
            : 0;
        gpuProfileRow.connect('notify::selected', () => {
            settings.set_string('gpu-profile', gpuProfileMap[gpuProfileRow.selected] ?? 'default');
        });
        gpuGroup.add(gpuProfileRow);

        const gpuWarningRow = new Adw.ActionRow({
            title: 'GPU profile changes take effect after the extension is disabled and re-enabled.',
            subtitle: 'Use the Extensions app to restart MilkDrop.',
            activatable: false,
        });
        const gpuWarningIcon = new Gtk.Image({
            icon_name: 'dialog-information-symbolic',
            valign: Gtk.Align.CENTER,
        });
        gpuWarningIcon.add_css_class('dim-label');
        gpuWarningRow.add_prefix(gpuWarningIcon);
        gpuGroup.add(gpuWarningRow);

        // ── Rendering ────────────────────────────────────────────────────────
        const renderingGroup = new Adw.PreferencesGroup({title: 'Rendering'});
        page.add(renderingGroup);

        const fpsRow = new Adw.SpinRow({
            title: 'Frame Rate',
            subtitle: 'Target frames per second',
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
        renderingGroup.add(fpsRow);

        // ── Presets ──────────────────────────────────────────────────────────
        const presetsGroup = new Adw.PreferencesGroup({title: 'Presets'});
        page.add(presetsGroup);

        const dirRow = new Adw.ActionRow({
            title: 'Directory',
            subtitle: 'Folder with preset files',
        });

        const dirLabel = new Gtk.Label({
            label: settings.get_string('preset-dir') || '(default)',
            xalign: 0,
            hexpand: true,
            ellipsize: Pango.EllipsizeMode.START,
            valign: Gtk.Align.CENTER,
        });
        dirLabel.add_css_class('dim-label');

        const presetDirSignalId = settings.connect('changed::preset-dir', () => {
            dirLabel.set_label(settings.get_string('preset-dir') || '(default)');
        });
        this._signalIds.push(presetDirSignalId);

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
        dirRow.activatable_widget = browseBtn;
        presetsGroup.add(dirRow);
    }

    _buildBehaviorPage(window, settings) {
        const page = new Adw.PreferencesPage({
            title: 'Behavior',
            icon_name: 'preferences-system-symbolic',
        });
        window.add(page);

        // ── Rotation ─────────────────────────────────────────────────────────
        const rotationGroup = new Adw.PreferencesGroup({title: 'Rotation'});
        page.add(rotationGroup);

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
        const shuffleSignalId = settings.connect('changed::shuffle', () => {
            const expected = settings.get_boolean('shuffle') ? 1 : 0;
            if (rotationRow.selected !== expected)
                rotationRow.selected = expected;
        });
        this._signalIds.push(shuffleSignalId);
        rotationGroup.add(rotationRow);

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
        rotationGroup.add(rotationIntervalRow);

        // ── Behavior ─────────────────────────────────────────────────────────
        const behaviorGroup = new Adw.PreferencesGroup({title: 'Behavior'});
        page.add(behaviorGroup);

        const overlayRow = new Adw.SwitchRow({
            title: 'Overlay Mode',
            subtitle: 'Display the visualizer over other windows',
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
            title: 'Pause on Maximized',
            subtitle: 'Pause when a maximized window is on the same monitor',
        });
        settings.bind('pause-on-maximized', pauseMaxRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(pauseMaxRow);

        const emptyDesktopRow = new Adw.SwitchRow({
            title: 'Pause on Empty Desktop',
            subtitle: 'Pause when no application windows are present on the active workspace',
        });
        settings.bind('pause-on-empty-desktop', emptyDesktopRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(emptyDesktopRow);

        const mediaAwareRow = new Adw.SwitchRow({
            title: 'Media-Aware Mode',
            subtitle: 'Pause when no media is playing',
        });
        settings.bind('media-aware', mediaAwareRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(mediaAwareRow);

        const stopIdleRow = new Adw.SwitchRow({
            title: 'Stop Renderer When Idle',
            subtitle: 'Fully stop the renderer to save resources when no media is playing',
        });
        settings.bind('stop-renderer-when-idle', stopIdleRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(stopIdleRow);
    }

    _buildTransitionsPage(window, settings) {
        const page = new Adw.PreferencesPage({
            title: 'Transitions',
            icon_name: 'emblem-music-symbolic',
        });
        window.add(page);

        // ── Transitions ──────────────────────────────────────────────────────
        const transitionsGroup = new Adw.PreferencesGroup({title: 'Transitions'});
        page.add(transitionsGroup);

        const beatSensRow = new Adw.ActionRow({
            title: 'Beat Sensitivity',
            subtitle: 'How strongly the visualizer reacts to beats',
        });
        const beatSensScale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 5.0, 0.1);
        beatSensScale.set_hexpand(true);
        beatSensScale.set_digits(1);
        beatSensScale.set_valign(Gtk.Align.CENTER);
        beatSensScale.set_draw_value(true);
        settings.bind('beat-sensitivity', beatSensScale, 'value', Gio.SettingsBindFlags.DEFAULT);
        beatSensRow.add_suffix(beatSensScale);
        beatSensRow.activatable_widget = beatSensScale;
        transitionsGroup.add(beatSensRow);

        const softCutRow = new Adw.SpinRow({
            title: 'Soft Cut Duration',
            subtitle: 'Duration of smooth preset transitions',
            adjustment: new Gtk.Adjustment({
                value: 3,
                lower: 1,
                upper: 30,
                step_increment: 1,
                page_increment: 5,
            }),
            digits: 0,
        });
        settings.bind('soft-cut-duration', softCutRow, 'value', Gio.SettingsBindFlags.DEFAULT);
        transitionsGroup.add(softCutRow);

        const hardCutExpander = new Adw.ExpanderRow({
            title: 'Hard Cuts',
            subtitle: 'Abrupt preset transitions on strong beats',
            show_enable_switch: true,
        });
        settings.bind('hard-cut-enabled', hardCutExpander, 'enable-expansion',
                      Gio.SettingsBindFlags.DEFAULT);
        transitionsGroup.add(hardCutExpander);

        const hardCutSensRow = new Adw.ActionRow({
            title: 'Sensitivity',
            subtitle: 'Required beat strength to trigger a transition',
        });
        const hardCutSensScale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 5.0, 0.1);
        hardCutSensScale.set_hexpand(true);
        hardCutSensScale.set_digits(1);
        hardCutSensScale.set_valign(Gtk.Align.CENTER);
        hardCutSensScale.set_draw_value(true);
        settings.bind('hard-cut-sensitivity', hardCutSensScale, 'value', Gio.SettingsBindFlags.DEFAULT);
        hardCutSensRow.add_suffix(hardCutSensScale);
        hardCutSensRow.activatable_widget = hardCutSensScale;
        hardCutExpander.add_row(hardCutSensRow);

        const hardCutDurRow = new Adw.SpinRow({
            title: 'Minimum Duration',
            subtitle: 'Minimum time before an abrupt transition',
            adjustment: new Gtk.Adjustment({
                value: 20,
                lower: 1,
                upper: 120,
                step_increment: 1,
                page_increment: 10,
            }),
            digits: 0,
        });
        settings.bind('hard-cut-duration', hardCutDurRow, 'value', Gio.SettingsBindFlags.DEFAULT);
        hardCutExpander.add_row(hardCutDurRow);
    }
}
