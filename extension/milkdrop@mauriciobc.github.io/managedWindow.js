/**
 * ManagedWindow — tracks one renderer Meta.Window anchored in the wallpaper layer.
 *
 * Based on Hanabi’s ManagedWindow pattern (hanabi/src/windowManager.js).
 * Encapsulates per-window state and signal wiring for predictable cleanup.
 */

import GLib from 'gi://GLib';

/**
 * Optional callbacks for window events.
 * @typedef {Object} WindowCallbacks
 * @property {function(MetaWindow):void} [onRaised]
 * @property {function(MetaWindow):void} [onMoved]
 * @property {function(MetaWindow):void} [onMinimized]
 * @property {function(MetaWindow):void} [onShown]
 */

/**
 * Internal window state snapshot.
 * @typedef {Object} WindowState
 * @property {boolean} keepAtBottom
 * @property {boolean} keepPosition
 * @property {boolean} keepMinimized - reserved for future “keep minimized” behaviour
 * @property {string} reparentState - 'window_group' | 'wallpaper' | null
 */

export class ManagedWindow {
    /**
     * @param {Meta.Window} metaWindow
     * @param {number} monitorIndex
     * @param {WindowCallbacks} [callbacks]
     */
    constructor(metaWindow, monitorIndex, callbacks = {}) {
        this._window = metaWindow;
        this._monitorIndex = monitorIndex;
        this._callbacks = callbacks;
        this._signals = [];
        this._disabled = false;

        /**
         * @type {WindowState}
         */
        this._state = {
            keepAtBottom: true,
            keepPosition: true,
            keepMinimized: false,
            reparentState: null,
        };

        this._connectSignals();
    }

    /** @private */
    _connectSignals() {
        /* Keep the renderer stacked at the bottom when something raises it. */
        const raisedId = this._window.connect_after('raised', () => {
            this._onRaised();
        });
        this._signals.push(raisedId);

        /* Track move/resize */
        const positionId = this._window.connect('position-changed', () => {
            this._onMoved();
        });
        this._signals.push(positionId);

        /* Minimize state */
        const minimizedId = this._window.connect('notify::minimized', () => {
            this._onMinimized();
        });
        this._signals.push(minimizedId);

        /* Shown */
        const shownId = this._window.connect('shown', () => {
            this._onShown();
        });
        this._signals.push(shownId);
    }

    /** @private */
    _onRaised() {
        if (this._disabled || !this._state.keepAtBottom)
            return;

        this._window.lower();

        const actor = this._window.get_compositor_private();
        if (actor && global.window_group && this._state.reparentState === 'window_group') {
            try {
                global.window_group.set_child_below_sibling(actor, null);
            } catch (e) {
                log(`[milkdrop] ManagedWindow: Error setting child position: ${e}`);
            }
        }

        if (this._callbacks.onRaised)
            this._callbacks.onRaised(this._window);
    }

    /** @private */
    _onMoved() {
        if (this._disabled || !this._state.keepPosition)
            return;

        if (this._callbacks.onMoved)
            this._callbacks.onMoved(this._window);
    }

    /** @private */
    _onMinimized() {
        if (this._disabled)
            return;

        if (this._callbacks.onMinimized)
            this._callbacks.onMinimized(this._window);
    }

    /** @private */
    _onShown() {
        if (this._disabled)
            return;

        if (this._callbacks.onShown)
            this._callbacks.onShown(this._window);
    }

    /** @returns {Meta.Window} */
    get window() {
        return this._window;
    }

    /** @returns {number} */
    get monitorIndex() {
        return this._monitorIndex;
    }

    /** @returns {WindowState} */
    get state() {
        return { ...this._state };
    }

    /** @param {string} key @param {any} value */
    setState(key, value) {
        if (key in this._state)
            this._state[key] = value;
    }

    /**
     * Checks if an actor is valid and can be operated on.
     * @param {Clutter.Actor} actor
     * @returns {boolean}
     */
    _isActorValid(actor) {
        if (!actor)
            return false;
        if (typeof actor.is_finalized === 'function' && actor.is_finalized())
            return false;
        if (typeof actor.is_destroyed === 'function' && actor.is_destroyed())
            return false;
        if (typeof actor.get_stage === 'function' && !actor.get_stage())
            return false;
        return typeof actor.get_parent === 'function';
    }

    /**
     * Anchor into the wallpaper layer: correct monitor, sticky, full coverage, lowered.
     * @param {Meta.Rectangle} geometry
     */
    anchor(geometry) {
        const actor = this._window.get_compositor_private();
        if (!this._isActorValid(actor)) {
            log(`[milkdrop] ManagedWindow: Invalid actor for anchoring on monitor ${this._monitorIndex}`);
            return;
        }

        const parent = actor.get_parent();
        const parentName = parent ? (parent._milkdropWallpaper ? `wallpaper(StWidget)` : String(parent)) : 'null';
        log(`[milkdrop] ManagedWindow: Actor parent: ${parentName}`);

        // If already in wallpaper (parent has _milkdropWallpaper === true), skip window_group anchoring
        if (parent && parent._milkdropWallpaper === true) {
            this._state.reparentState = 'wallpaper';
            log('[milkdrop] ManagedWindow: Actor already in wallpaper, skipping window_group anchoring');
        } else {
            this._state.reparentState = 'window_group';
            // Move actor to the bottom of the window group
            try {
                if (parent && parent !== global.window_group)
                    parent.remove_child(actor);
                if (actor.get_parent() !== global.window_group)
                    global.window_group.add_child(actor);
                global.window_group.set_child_below_sibling(actor, null);
                log('[milkdrop] ManagedWindow: Actor moved to bottom of window_group');
            } catch (e) {
                log(`[milkdrop] ManagedWindow: Error moving actor to window_group: ${e}`);
                return;
            }
        }

        this._window.stick();
        log('[milkdrop] ManagedWindow: Window stuck to all workspaces');

        this.enforceCoverage(geometry);

        this._window.lower();
        log('[milkdrop] ManagedWindow: Window lowered in Meta stack');
    }

    /** @param {Meta.Rectangle} geometry */
    enforceCoverage(geometry) {
        if (!geometry || geometry.width <= 0 || geometry.height <= 0) {
            log(`[milkdrop] ManagedWindow: Invalid geometry for monitor ${this._monitorIndex}`);
            return;
        }

        try {
            this._window.move_to_monitor(this._monitorIndex);
            log(`[milkdrop] ManagedWindow: move_to_monitor(${this._monitorIndex}) succeeded`);
        } catch (e) {
            log(`[milkdrop] ManagedWindow: move_to_monitor failed: ${e}`);
        }

        try {
            this._window.move_resize_frame(
                false,
                geometry.x,
                geometry.y,
                geometry.width,
                geometry.height
            );
            log(`[milkdrop] ManagedWindow: move_resize_frame to ${geometry.width}x${geometry.height} succeeded`);
        } catch (e) {
            log(`[milkdrop] ManagedWindow: move_resize_frame failed: ${e}`);
        }
    }

    /** Disconnect signals and clear references. */
    disable() {
        this._disabled = true;
        
        for (const signalId of this._signals) {
            try {
                this._window.disconnect(signalId);
            } catch (e) {
                /* ignore — window may already be gone */
            }
        }
        this._signals = [];
        this._window = null;
        this._callbacks = {};
    }
}
