import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';
import {queryMilkdropStatus} from './controlClient.js';

export default class MilkdropPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();

        const page = new Adw.PreferencesPage({
            title: 'Milkdrop',
            icon_name: 'applications-multimedia-symbolic',
        });
        window.add(page);

        // --- Status group (live read-only view, polls every 2 s while window is open) ---
        const statusGroup = new Adw.PreferencesGroup({title: 'Status'});
        page.add(statusGroup);

        const makeStatusRow = (title, initial) => {
            const row = new Adw.ActionRow({title});
            const label = new Gtk.Label({label: initial, xalign: 1.0, hexpand: true});
            label.add_css_class('dim-label');
            row.add_suffix(label);
            statusGroup.add(row);
            return label;
        };

        const fpsLabel = makeStatusRow('FPS', '—');
        const presetLabel = makeStatusRow('Preset', '—');
        const audioLabel = makeStatusRow('Audio', '—');
        const quarantineLabel = makeStatusRow('Quarantined presets', '—');
        const pausedLabel = makeStatusRow('Paused', '—');

        let pollSourceId = 0;

        const refreshStatus = () => {
            queryMilkdropStatus().then(status => {
                if (!status) {
                    fpsLabel.set_label('—');
                    presetLabel.set_label('—');
                    audioLabel.set_label('—');
                    quarantineLabel.set_label('—');
                    pausedLabel.set_label('—');
                    return;
                }
                fpsLabel.set_label(Number.isFinite(status.fps) ? status.fps.toFixed(1) : '—');
                presetLabel.set_label(status.preset || '(none)');
                audioLabel.set_label(status.audio ?? '—');
                quarantineLabel.set_label(String(status.quarantine ?? 0));
                pausedLabel.set_label(status.paused ? 'Yes' : 'No');
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

        // --- Runtime group ---
        const runtimeGroup = new Adw.PreferencesGroup({title: 'Runtime'});
        page.add(runtimeGroup);

        const enabledRow = new Adw.SwitchRow({title: 'Enabled'});
        settings.bind('enabled', enabledRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        runtimeGroup.add(enabledRow);

        const monitorRow = new Adw.ActionRow({title: 'Monitor index'});
        const monitorSpin = Gtk.SpinButton.new_with_range(0, 16, 1);
        settings.bind('monitor', monitorSpin, 'value', Gio.SettingsBindFlags.DEFAULT);
        monitorRow.add_suffix(monitorSpin);
        monitorRow.activatable_widget = monitorSpin;
        runtimeGroup.add(monitorRow);

        const opacityRow = new Adw.ActionRow({title: 'Opacity'});
        const opacityScale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 1.0, 0.01);
        opacityScale.set_hexpand(true);
        opacityScale.set_digits(2);
        settings.bind('opacity', opacityScale, 'value', Gio.SettingsBindFlags.DEFAULT);
        opacityRow.add_suffix(opacityScale);
        opacityRow.activatable_widget = opacityScale;
        runtimeGroup.add(opacityRow);

        const behaviorGroup = new Adw.PreferencesGroup({title: 'Behavior'});
        page.add(behaviorGroup);

        const shuffleRow = new Adw.SwitchRow({title: 'Shuffle presets'});
        settings.bind('shuffle', shuffleRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(shuffleRow);

        const overlayRow = new Adw.SwitchRow({
            title: 'Overlay mode',
            subtitle: 'Renderer flag and control-socket state (see status); extra visuals may come later.',
        });
        settings.bind('overlay', overlayRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(overlayRow);

        const pauseFsRow = new Adw.SwitchRow({
            title: 'Pause when fullscreen app is present',
            subtitle: 'Reduces GPU usage when a fullscreen window covers the visualizer.',
        });
        settings.bind('pause-on-fullscreen', pauseFsRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(pauseFsRow);

        const pauseMaxRow = new Adw.SwitchRow({
            title: 'Pause when maximized app is present',
            subtitle: 'Pauses when any maximized (non-fullscreen) window is on the same monitor.',
        });
        settings.bind('pause-on-maximized', pauseMaxRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(pauseMaxRow);

        const mediaAwareRow = new Adw.SwitchRow({
            title: 'Media-aware mode',
            subtitle: 'Pause when no MPRIS media player is playing.',
        });
        settings.bind('media-aware', mediaAwareRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        behaviorGroup.add(mediaAwareRow);

        const presetGroup = new Adw.PreferencesGroup({title: 'Presets'});
        page.add(presetGroup);

        const presetRow = new Adw.ActionRow({title: 'Preset directory'});
        const presetEntry = new Gtk.Entry({hexpand: true});
        settings.bind('preset-dir', presetEntry, 'text', Gio.SettingsBindFlags.DEFAULT);
        presetRow.add_suffix(presetEntry);
        presetRow.activatable_widget = presetEntry;
        presetGroup.add(presetRow);
    }
}