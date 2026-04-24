import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const MPRIS_PREFIX = 'org.mpris.MediaPlayer2.';
const MPRIS_PLAYER_IFACE = 'org.mpris.MediaPlayer2.Player';
const MPRIS_OBJECT_PATH = '/org/mpris/MediaPlayer2';
const PROPS_IFACE = 'org.freedesktop.DBus.Properties';
const DBUS_NAME = 'org.freedesktop.DBus';
const DBUS_PATH = '/org/freedesktop/DBus';

/**
 * MprisWatcher monitors all active MPRIS media players on the session bus
 * and invokes onStateChange(isAnyPlaying) whenever the aggregate playback
 * state changes.
 *
 * Lifecycle: call enable() once after construction, disable() to tear down.
 */
export class MprisWatcher {
    constructor(onStateChange) {
        this._onStateChange = onStateChange;

        // busName → D-Bus signal subscription ID
        this._players = new Map();
        // busName → 'Playing' | 'Paused' | 'Stopped'
        this._states = new Map();
        // Cached count of players in 'Playing' state — avoids array spread on every query.
        this._playingCount = 0;

        this._nameChangedSubscriptionId = 0;
    }

    enable() {
        // Subscribe to NameOwnerChanged to detect players appearing/disappearing.
        this._nameChangedSubscriptionId = Gio.DBus.session.signal_subscribe(
            DBUS_NAME,
            DBUS_NAME,
            'NameOwnerChanged',
            DBUS_PATH,
            null,
            Gio.DBusSignalFlags.NONE,
            (_conn, _sender, _path, _iface, _signal, params) => {
                const [name, oldOwner, newOwner] = params.deepUnpack();
                if (!name.startsWith(MPRIS_PREFIX))
                    return;
                if (newOwner && !oldOwner)
                    this._addPlayer(name);
                else if (oldOwner && !newOwner)
                    this._removePlayer(name);
            }
        );

        // Enumerate players already on the bus.
        Gio.DBus.session.call(
            DBUS_NAME,
            DBUS_PATH,
            DBUS_NAME,
            'ListNames',
            null,
            new GLib.VariantType('(as)'),
            Gio.DBusCallFlags.NONE,
            -1,
            null,
            (conn, res) => {
                let result;
                try {
                    result = conn.call_finish(res);
                } catch (e) {
                    log(`[milkdrop] MprisWatcher: ListNames failed: ${e.message}`);
                    return;
                }
                const [names] = result.deepUnpack();
                for (const name of names) {
                    if (name.startsWith(MPRIS_PREFIX))
                        this._addPlayer(name);
                }
            }
        );
    }

    disable() {
        if (this._nameChangedSubscriptionId) {
            Gio.DBus.session.signal_unsubscribe(this._nameChangedSubscriptionId);
            this._nameChangedSubscriptionId = 0;
        }

        for (const subscriptionId of this._players.values())
            Gio.DBus.session.signal_unsubscribe(subscriptionId);
        this._players.clear();
        this._states.clear();
        this._playingCount = 0;
    }

    get isAnyPlaying() {
        return this._playingCount > 0;
    }

    /**
     * GJS serializes all D-Bus callbacks on the main loop. Both the
     * PropertiesChanged signal handler and the async Get callback update
     * this._states before reading wasPlaying, so whichever runs second
     * sees the state written by the first and correctly skips the count.
     */
    _addPlayer(busName) {
        if (this._players.has(busName))
            return;

        // Subscribe to PropertiesChanged to track PlaybackStatus changes.
        const subscriptionId = Gio.DBus.session.signal_subscribe(
            busName,
            PROPS_IFACE,
            'PropertiesChanged',
            MPRIS_OBJECT_PATH,
            MPRIS_PLAYER_IFACE,
            Gio.DBusSignalFlags.NONE,
            (_conn, _sender, _path, _iface, _signal, params) => {
                const [changedIface, changedProps] = params.deepUnpack();
                if (changedIface !== MPRIS_PLAYER_IFACE)
                    return;
                if ('PlaybackStatus' in changedProps) {
                    const status = changedProps['PlaybackStatus'].deepUnpack();
                    const wasPlaying = this._states.get(busName) === 'Playing';
                    const isPlaying = status === 'Playing';
                    if (wasPlaying !== isPlaying)
                        this._playingCount += isPlaying ? 1 : -1;
                    this._states.set(busName, status);
                    this._onStateChange(this.isAnyPlaying);
                }
            }
        );

        this._players.set(busName, subscriptionId);

        // Read initial PlaybackStatus.
        Gio.DBus.session.call(
            busName,
            MPRIS_OBJECT_PATH,
            PROPS_IFACE,
            'Get',
            new GLib.Variant('(ss)', [MPRIS_PLAYER_IFACE, 'PlaybackStatus']),
            new GLib.VariantType('(v)'),
            Gio.DBusCallFlags.NONE,
            1000,
            null,
            (conn, res) => {
                let result;
                try {
                    result = conn.call_finish(res);
                } catch (_e) {
                    // Player may have disappeared between discovery and the Get call.
                    return;
                }
                const [variant] = result.deepUnpack();
                const status = variant.deepUnpack();
                const wasPlaying = this._states.get(busName) === 'Playing';
                const isPlaying = status === 'Playing';
                if (wasPlaying !== isPlaying)
                    this._playingCount += isPlaying ? 1 : -1;
                this._states.set(busName, status);
                this._onStateChange(this.isAnyPlaying);
            }
        );
    }

    _removePlayer(busName) {
        const subscriptionId = this._players.get(busName);
        if (subscriptionId !== undefined) {
            Gio.DBus.session.signal_unsubscribe(subscriptionId);
            this._players.delete(busName);
        }
        if (this._states.get(busName) === 'Playing')
            this._playingCount--;
        this._states.delete(busName);
        this._onStateChange(this.isAnyPlaying);
    }
}
