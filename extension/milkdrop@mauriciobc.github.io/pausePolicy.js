import Meta from 'gi://Meta';

import {PAUSE_REASON_FULLSCREEN, PAUSE_REASON_MAXIMIZED} from './constants.js';

/**
 * PausePolicy tracks whether a covering window (fullscreen or maximized) is
 * present on the renderer's monitor and invokes setPauseReason() accordingly.
 *
 * setPauseReason(reason, active) is provided by the extension and coordinates
 * all pause reasons into a single `pause on` / `pause off` socket command.
 */
export class PausePolicy {
    constructor(monitorIndex, settings, setPauseReason) {
        this._monitorIndex = monitorIndex;
        this._settings = settings;
        this._setPauseReason = setPauseReason;

        this._displaySignalIds = [];
        this._windowSignals = new Map(); // MetaWindow → [signalId, ...]
    }

    enable() {
        // Fullscreen: Meta.Display emits 'in-fullscreen-changed' whenever any
        // monitor's fullscreen state changes — no per-window tracking needed.
        this._displaySignalIds.push(
            global.display.connect('in-fullscreen-changed', () => {
                this._evaluateFullscreen();
            })
        );

        // Maximized: track each window individually since there is no
        // display-level maximized-changed signal.
        this._displaySignalIds.push(
            global.display.connect('window-created', (_display, win) => {
                this._trackWindow(win);
                this._evaluateMaximized();
            })
        );

        // Track windows already present when policy is enabled.
        for (const actor of global.get_window_actors(false)) {
            const win = actor.meta_window;
            if (win)
                this._trackWindow(win);
        }

        // Initial evaluation.
        this._evaluateFullscreen();
        this._evaluateMaximized();
    }

    disable() {
        for (const id of this._displaySignalIds)
            global.display.disconnect(id);
        this._displaySignalIds = [];

        for (const [win, ids] of this._windowSignals) {
            for (const id of ids) {
                try { win.disconnect(id); } catch (_e) { /* already unmanaged */ }
            }
        }
        this._windowSignals.clear();

        // Clear both reasons so the extension resumes on disable.
        this._setPauseReason(PAUSE_REASON_FULLSCREEN, false);
        this._setPauseReason(PAUSE_REASON_MAXIMIZED, false);
    }

    _trackWindow(win) {
        if (!win || this._windowSignals.has(win))
            return;

        const ids = [];

        ids.push(win.connect('notify::maximized-vertically', () => {
            this._evaluateMaximized();
        }));

        ids.push(win.connect('notify::maximized-horizontally', () => {
            this._evaluateMaximized();
        }));

        ids.push(win.connect('notify::minimized', () => {
            this._evaluateMaximized();
        }));

        ids.push(win.connect('unmanaged', () => {
            const storedIds = this._windowSignals.get(win);
            if (storedIds) {
                for (const id of storedIds) {
                    try { win.disconnect(id); } catch (_e) { /* already gone */ }
                }
                this._windowSignals.delete(win);
            }
            this._evaluateMaximized();
        }));

        this._windowSignals.set(win, ids);
    }

    _evaluateFullscreen() {
        if (!this._settings.get_boolean('pause-on-fullscreen')) {
            this._setPauseReason(PAUSE_REASON_FULLSCREEN, false);
            return;
        }

        const inFullscreen = global.display.get_monitor_in_fullscreen(this._monitorIndex);
        this._setPauseReason(PAUSE_REASON_FULLSCREEN, inFullscreen);
    }

    _evaluateMaximized() {
        if (!this._settings.get_boolean('pause-on-maximized')) {
            this._setPauseReason(PAUSE_REASON_MAXIMIZED, false);
            return;
        }

        let hasMaximized = false;
        for (const actor of global.get_window_actors(false)) {
            const w = actor.meta_window;
            if (!w)
                continue;

            if (typeof w.get_compositor_private !== 'function')
                continue;
            if (w.get_compositor_private() === null)
                continue;
            if (w.get_monitor() !== this._monitorIndex)
                continue;
            if (w.minimized || w.fullscreen)
                continue;

            if (w.maximized_vertically || w.maximized_horizontally) {
                hasMaximized = true;
                break;
            }
        }

        this._setPauseReason(PAUSE_REASON_MAXIMIZED, hasMaximized);
    }

    /**
     * Re-evaluate both policies — called when a relevant GSettings key changes.
     */
    reEvaluate() {
        this._evaluateFullscreen();
        this._evaluateMaximized();
    }
}
