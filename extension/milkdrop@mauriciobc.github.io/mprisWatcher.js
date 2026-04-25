import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const MPRIS_PREFIX = 'org.mpris.MediaPlayer2.';
const MPRIS_PATH = '/org/mpris/MediaPlayer2';
const PLAYER_IFACE = 'org.mpris.MediaPlayer2.Player';
const PLAYBACK_STATUS = 'PlaybackStatus';
const DBUS_NAME = 'org.freedesktop.DBus';
const DBUS_PATH = '/org/freedesktop/DBus';

const MPRIS_PLAYER_IFACE_XML = `
<node>
  <interface name="${PLAYER_IFACE}">
    <property name="${PLAYBACK_STATUS}" type="s" access="read"/>
  </interface>
</node>`;

let MprisPlayerProxy;
try {
    MprisPlayerProxy = Gio.DBusProxy.makeProxyWrapper(MPRIS_PLAYER_IFACE_XML);
} catch (e) {
    log(`[milkdrop] MprisWatcher: makeProxyWrapper failed: ${e.message}. MPRIS support disabled.`);
    MprisPlayerProxy = null;
}

function _debugMpris() {
    return GLib.getenv('MILKDROP_DEBUG_MPRIS') === '1';
}

/**
 * Recursively unwrap GJS GVariant values.
 * In GJS, PropertiesChanged a{sv} values may remain as GVariant
 * and need recursive unpacking before string comparison.
 */
function _deepUnpack(value) {
    if (value == null)
        return value;
    if (typeof value.recursiveUnpack === 'function')
        return _deepUnpack(value.recursiveUnpack());
    return value;
}

/**
 * Parse PlaybackStatus from a proxy property or PropertiesChanged a{sv} value.
 * Returns { statusStr, playing } where playing is true only when statusStr === 'Playing'.
 */
function _parsePlaybackStatus(unpacked) {
    const statusStr = _deepUnpack(unpacked);
    return { statusStr, playing: statusStr === 'Playing' };
}

/**
 * MprisWatcher monitors all active MPRIS media players on the session bus
 * and invokes onStateChange(isAnyPlaying) whenever the aggregate playback
 * state changes.
 *
 * Uses Gio.DBusProxy per player for automatic property caching and
 * g-properties-changed signal delivery.
 *
 * Lifecycle: call enable() once after construction, disable() to tear down.
 */
export class MprisWatcher {
    constructor(onStateChange) {
        this._onStateChange = onStateChange;

        this._connection = null;
        this._nameChangedSubscriptionId = 0;
        this._playerProxies = new Map();      // busName -> MprisPlayerProxy
        this._pendingInit = new Set();        // busNames with in-flight proxy creation
        this._playingByBusName = new Map();   // busName -> boolean
        this._playingCount = 0;               // cached count for O(1) isAnyPlaying
        this._enabled = false;
        this._hasEmittedInitialState = false;
    }

    enable() {
        if (this._enabled)
            return;

        const connection = Gio.DBus.session;
        if (!connection) {
            log('[milkdrop] MprisWatcher: no session bus available');
            return;
        }

        this._enabled = true;
        this._connection = connection;

        if (_debugMpris())
            log('[milkdrop] MprisWatcher: enable()');

        this._nameChangedSubscriptionId = connection.signal_subscribe(
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

        this._listAndAddPlayers();
    }

    disable() {
        if (!this._enabled)
            return;

        this._enabled = false;
        this._hasEmittedInitialState = false;

        if (this._nameChangedSubscriptionId && this._connection) {
            try {
                this._connection.signal_unsubscribe(this._nameChangedSubscriptionId);
            } catch (_e) {}
            this._nameChangedSubscriptionId = 0;
        }

        this._pendingInit.clear();

        for (const proxy of this._playerProxies.values())
            this._disconnectPlayerProxy(proxy);
        this._playerProxies.clear();

        this._playingByBusName.clear();
        this._playingCount = 0;
        this._connection = null;

        if (_debugMpris())
            log('[milkdrop] MprisWatcher: disable()');
    }

    get isAnyPlaying() {
        return this._playingCount > 0;
    }

    _setPlaying(busName, playing) {
        const wasPlaying = this._playingByBusName.get(busName) ?? false;
        if (wasPlaying === playing)
            return;
        this._playingByBusName.set(busName, playing);
        this._playingCount = Math.max(0, this._playingCount + (playing ? 1 : -1));
    }

    _onPlayingChanged(busName, playing, reason) {
        const previous = this.isAnyPlaying;
        this._setPlaying(busName, playing);
        const changed = previous !== this.isAnyPlaying;
        if (changed || (reason === 'added' && !this._hasEmittedInitialState)) {
            if (_debugMpris()) {
                const short = busName.replace(MPRIS_PREFIX, '');
                const transition = changed ? `${previous} → ${this.isAnyPlaying}` : `${this.isAnyPlaying} (initial)`;
                log(`[milkdrop] MprisWatcher: isAnyPlaying ${transition} (${reason} ${short})`);
            }
            this._onStateChange(this.isAnyPlaying);
            this._hasEmittedInitialState = true;
        }
    }

    _listAndAddPlayers() {
        if (!this._connection || !this._enabled)
            return;

        try {
            this._connection.call(
                DBUS_NAME,
                DBUS_PATH,
                DBUS_NAME,
                'ListNames',
                null,
                null,
                Gio.DBusCallFlags.NONE,
                -1,
                null,
                (connection, result) => {
                    if (!this._enabled)
                        return;
                    try {
                        const reply = connection.call_finish(result);
                        if (!reply) {
                            log('[milkdrop] MprisWatcher: ListNames returned null reply');
                            return;
                        }
                        const names = reply.recursiveUnpack?.().flat?.() ?? [];
                        const mprisNames = new Set(
                            names.filter(n => typeof n === 'string' && n.startsWith(MPRIS_PREFIX))
                        );

                        for (const busName of mprisNames) {
                            if (!this._playerProxies.has(busName) && !this._pendingInit.has(busName))
                                this._addPlayer(busName);
                        }

                        for (const busName of [...this._playerProxies.keys()]) {
                            if (!mprisNames.has(busName))
                                this._removePlayer(busName);
                        }

                        const count = this._playerProxies.size + this._pendingInit.size;
                        if (_debugMpris())
                            log(`[milkdrop] MprisWatcher: enabled, ${count} player(s), isAnyPlaying=${this.isAnyPlaying}`);

                        if (mprisNames.size === 0 && !this._hasEmittedInitialState) {
                            if (_debugMpris())
                                log('[milkdrop] MprisWatcher: no players found, emitting initial state isAnyPlaying=false');
                            this._onStateChange(this.isAnyPlaying);
                            this._hasEmittedInitialState = true;
                        }
                    } catch (e) {
                        log(`[milkdrop] MprisWatcher: ListNames error: ${e.message}`);
                    }
                }
            );
        } catch (e) {
            log(`[milkdrop] MprisWatcher: ListNames call failed: ${e.message}`);
        }
    }

    _addPlayer(busName) {
        if (!MprisPlayerProxy) {
            log('[milkdrop] MprisWatcher: cannot add player, proxy class unavailable');
            return;
        }
        if (this._playerProxies.has(busName) || this._pendingInit.has(busName))
            return;

        this._pendingInit.add(busName);

        new MprisPlayerProxy(
            this._connection,
            busName,
            MPRIS_PATH,
            (proxy, error) => {
                // If _removePlayer was called while proxy init was in-flight,
                // delete returns false and we bail out. Otherwise we proceed.
                const wasPending = this._pendingInit.delete(busName);
                if (!this._enabled || !wasPending)
                    return;
                if (error) {
                    log(`[milkdrop] MprisWatcher: proxy error for ${busName.replace(MPRIS_PREFIX, '')}: ${error.message}`);
                    return;
                }
                if (this._playerProxies.has(busName))
                    return;

                let propsChangedId = 0;
                let nameOwnerNotifyId = 0;
                try {
                    const statusStr = proxy.PlaybackStatus ?? '';
                    const { playing } = _parsePlaybackStatus(statusStr);

                    propsChangedId = proxy.connect('g-properties-changed',
                        (_pxy, changed, _invalidated) => this._onPropertiesChanged(busName, changed));
                    nameOwnerNotifyId = proxy.connect('notify::g-name-owner',
                        () => {
                            if (!proxy.g_name_owner)
                                this._removePlayer(busName);
                        });

                    proxy._propsChangedId = propsChangedId;
                    proxy._nameOwnerNotifyId = nameOwnerNotifyId;

                    this._playerProxies.set(busName, proxy);

                    this._onPlayingChanged(busName, playing, 'added');

                    if (_debugMpris()) {
                        const short = busName.replace(MPRIS_PREFIX, '');
                        log(`[milkdrop] MprisWatcher: proxy ready ${short}: PlaybackStatus=${statusStr || '(empty)'} playing=${playing}`);
                    }
                } catch (e) {
                    if (propsChangedId)
                        try { proxy.disconnect(propsChangedId); } catch (_e) {}
                    if (nameOwnerNotifyId)
                        try { proxy.disconnect(nameOwnerNotifyId); } catch (_e) {}
                    log(`[milkdrop] MprisWatcher: proxy setup failed for ${busName.replace(MPRIS_PREFIX, '')}: ${e.message}`);
                }
            },
            null,
            Gio.DBusProxyFlags.DO_NOT_AUTO_START
        );
    }

    _removePlayer(busName) {
        const previous = this.isAnyPlaying;

        this._pendingInit.delete(busName);

        const proxy = this._playerProxies.get(busName);
        if (proxy)
            this._disconnectPlayerProxy(proxy);
        this._playerProxies.delete(busName);

        const wasPlaying = this._playingByBusName.get(busName) ?? false;
        this._playingByBusName.delete(busName);
        if (wasPlaying)
            this._playingCount = Math.max(0, this._playingCount - 1);

        if (_debugMpris())
            log(`[milkdrop] MprisWatcher: removed ${busName.replace(MPRIS_PREFIX, '')}`);

        if (previous !== this.isAnyPlaying)
            this._onStateChange(this.isAnyPlaying);
    }

    _disconnectPlayerProxy(proxy) {
        try {
            if (proxy._propsChangedId) {
                proxy.disconnect(proxy._propsChangedId);
                proxy._propsChangedId = 0;
            }
            if (proxy._nameOwnerNotifyId) {
                proxy.disconnect(proxy._nameOwnerNotifyId);
                proxy._nameOwnerNotifyId = 0;
            }
        } catch (_e) {}
    }

    _onPropertiesChanged(busName, changed) {
        const changedJS = changed.recursiveUnpack?.() ?? {};
        if (!Object.prototype.hasOwnProperty.call(changedJS, PLAYBACK_STATUS))
            return;

        const { statusStr, playing } = _parsePlaybackStatus(changedJS[PLAYBACK_STATUS]);
        if (_debugMpris()) {
            const short = busName.replace(MPRIS_PREFIX, '');
            log(`[milkdrop] MprisWatcher: PropertiesChanged ${short}: PlaybackStatus=${statusStr || '(empty)'} playing=${playing}`);
        }

        this._onPlayingChanged(busName, playing, 'PropertiesChanged');
    }
}
