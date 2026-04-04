import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

export default class MilkdropPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();

        const page = new Adw.PreferencesPage({
            title: 'Milkdrop',
            icon_name: 'applications-multimedia-symbolic',
        });
        window.add(page);

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