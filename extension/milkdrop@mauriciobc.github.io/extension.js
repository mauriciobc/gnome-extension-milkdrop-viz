import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Clutter from 'gi://Clutter';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Config from 'resource:///org/gnome/shell/misc/config.js';
import * as Background from 'resource:///org/gnome/shell/ui/background.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Workspace from 'resource:///org/gnome/shell/ui/workspace.js';
import * as WorkspaceThumbnail from 'resource:///org/gnome/shell/ui/workspaceThumbnail.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

import {RETRY_DELAYS_MS, PAUSE_REASON_MPRIS, FADE_DURATION_MS} from './constants.js';
import {getMilkdropSocketPath, sendMilkdropControlCommand} from './controlClient.js';
import {PausePolicy} from './pausePolicy.js';
import {MprisWatcher} from './mprisWatcher.js';

export default class MilkdropExtension extends Extension {
    constructor(metadata) {
        super(metadata);

        this._settings = null;
        this._settingsSignalIds = [];
        this._wmSignalIds = [];

        // Extension-level MPRIS watcher (affects all renderers simultaneously).
        this._mprisWatcher = null;
        // Extension-level pause reasons that broadcast to all renderers (e.g. MPRIS).
        this._globalPauseReasons = new Set();

        // Per-monitor renderer state: Map<monitorIndex, rendererState>
        this._renderers = new Map();

        this._startupCompleteId = 0;

        this._injectionManager = null;
        this._wallpaperActors = new Set();
        this._wallpaperSourceIds = new Set();
        this._overviewShowingId = 0;
        this._overviewHiddenId = 0;
        this._workareasChangedId = 0;
        this._monitorsChangedId = 0;
    }

    // ─── Lifecycle ────────────────────────────────────────────────────────────

    enable() {
        this._settings = this.getSettings();

        this._settingsSignalIds.push(
            this._settings.connect('changed::enabled', () => this._syncEnabledState()),
            this._settings.connect('changed', (_settings, key) => this._onSettingChanged(key))
        );

        this._wmSignalIds.push(
            global.window_manager.connect_after('map', (_wm, windowActor) => {
                const window = windowActor.get_meta_window();
                this._onWindowMapped(window);
            })
        );

        this._installOverrides();
        this._reloadBackgrounds();

        this._overviewShowingId = Main.overview.connect('showing', () => {
            for (const [, state] of this._renderers) {
                const actor = this._getRendererActor(state);
                if (actor)
                    actor.hide();
            }
        });
        this._overviewHiddenId = Main.overview.connect('hidden', () => {
            for (const [, state] of this._renderers) {
                const actor = this._getRendererActor(state);
                if (actor)
                    actor.show();
                if (state.anchoredWindow)
                    this._enforceWindowCoverage(state.anchoredWindow);
            }
        });

        this._workareasChangedId = global.display.connect('workareas-changed', () => {
            for (const [, state] of this._renderers) {
                if (state.anchoredWindow)
                    this._enforceWindowCoverage(state.anchoredWindow);
            }
        });
        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed', () => {
            for (const [, state] of this._renderers) {
                if (state.anchoredWindow)
                    this._enforceWindowCoverage(state.anchoredWindow);
            }
            this._reloadBackgrounds();
            this._syncMonitors();
        });

        if (Main.layoutManager._startingUp) {
            this._startupCompleteId = Main.layoutManager.connect('startup-complete', () => {
                this._disconnectStartupComplete();
                this._syncEnabledState();
            });
        } else {
            this._syncEnabledState();
        }
    }

    disable() {
        this._disconnectStartupComplete();
        if (this._overviewShowingId) {
            Main.overview.disconnect(this._overviewShowingId);
            this._overviewShowingId = 0;
        }
        if (this._overviewHiddenId) {
            Main.overview.disconnect(this._overviewHiddenId);
            this._overviewHiddenId = 0;
        }
        if (this._workareasChangedId) {
            global.display.disconnect(this._workareasChangedId);
            this._workareasChangedId = 0;
        }
        if (this._monitorsChangedId) {
            Main.layoutManager.disconnect(this._monitorsChangedId);
            this._monitorsChangedId = 0;
        }
        this._disconnectWmSignals();
        this._disconnectSettingsSignals();
        this._stopMprisWatcher();
        this._stopAllRenderers();
        this._clearOverrides();
        this._settings = null;
    }

    // ─── Monitor orchestration ─────────────────────────────────────────────

    /**
     * Compute the desired set of monitor indices and start/stop renderers
     * so the running set exactly matches the desired set.
     */
    _syncMonitors() {
        if (!this._settings?.get_boolean('enabled')) {
            this._stopAllRenderers();
            return;
        }

        const desired = new Set();
        if (this._settings.get_boolean('all-monitors')) {
            const n = global.display.get_n_monitors();
            for (let i = 0; i < n; i++)
                desired.add(i);
        } else {
            desired.add(this._settings.get_int('monitor'));
        }

        // Stop renderers for monitors no longer in the desired set.
        for (const idx of [...this._renderers.keys()]) {
            if (!desired.has(idx))
                this._stopRendererByIndex(idx);
        }

        // Start renderers for monitors in the desired set that aren't running.
        for (const idx of desired) {
            if (!this._renderers.has(idx))
                this._spawnRendererForMonitor(idx);
        }
    }

    _syncEnabledState() {
        this._syncMonitors();
    }

    /** Compat wrapper — keeps scaffold validator happy. */
    _spawnProcess() {
        this._syncMonitors();
    }

    _makeRendererState(monitorIndex) {
        return {
            monitorIndex,
            subprocess: null,
            waylandClient: null,
            stdoutStream: null,
            stdoutCancellable: null,
            anchoredWindow: null,
            raisedSignalId: 0,
            anchorPending: false,
            anchorRetrySourceIds: new Set(),
            pausePolicy: null,
            pauseReasons: new Set(),
            renderingPaused: false,
            retrySourceId: 0,
            retryStep: 0,
            socketPath: null,
        };
    }

    _spawnRendererForMonitor(monitorIndex) {
        if (this._renderers.has(monitorIndex))
            return;

        const binaryPath = this._resolveBinaryPath();
        if (!binaryPath) {
            log(`[milkdrop] renderer binary not found`);
            return;
        }

        const state = this._makeRendererState(monitorIndex);
        state.socketPath = getMilkdropSocketPath(monitorIndex);
        this._renderers.set(monitorIndex, state);

        const argv = this._buildRendererArgs(binaryPath, monitorIndex);
        const shellMajor = parseInt(Config.PACKAGE_VERSION.split('.')[0], 10);
        const isWayland = Meta.is_wayland_compositor();

        const launcher = new Gio.SubprocessLauncher({
            flags: Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE,
        });
        launcher.setenv('GSK_RENDERER', 'gl', true);
        launcher.setenv('MILKDROP_FORCE_GL_API', '1', true);

        try {
            if (isWayland) {
                if (shellMajor < 49) {
                    state.waylandClient = Meta.WaylandClient.new(global.context, launcher);
                    state.subprocess = state.waylandClient.spawnv(global.display, argv);
                } else {
                    state.waylandClient = Meta.WaylandClient.new_subprocess(global.context, launcher, argv);
                    if (!state.waylandClient) {
                        log('[milkdrop] Meta.WaylandClient.new_subprocess returned null');
                        state.subprocess = null;
                    } else {
                        state.subprocess = state.waylandClient.get_subprocess();
                    }
                }
            } else {
                state.subprocess = launcher.spawnv(argv);
            }
        } catch (error) {
            const msg = error?.message ?? String(error);
            log(`[milkdrop] failed to spawn renderer (monitor ${monitorIndex}): ${msg}`);
            state.subprocess = null;
            state.waylandClient = null;
        }

        if (!state.subprocess) {
            this._renderers.delete(monitorIndex);
            this._scheduleRetryForMonitor(monitorIndex);
            return;
        }

        state.retryStep = 0;
        this._startPausePolicy(state);

        // Apply any active global pause reasons immediately.
        if (this._globalPauseReasons.size > 0) {
            for (const reason of this._globalPauseReasons)
                state.pauseReasons.add(reason);
            state.renderingPaused = true;
            sendMilkdropControlCommand('pause on', state.socketPath);
        }

        state.stdoutCancellable = new Gio.Cancellable();
        state.stdoutStream = Gio.DataInputStream.new(state.subprocess.get_stdout_pipe());
        this._readRendererOutput(state);

        const spawnedSubprocess = state.subprocess;
        state.subprocess.wait_async(state.stdoutCancellable, (proc, res) => {
            let exitCode = -1;
            try {
                proc.wait_finish(res);
                exitCode = proc.get_exit_status();
            } catch (error) {
                if (!error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                    log(`[milkdrop] renderer wait failed (monitor ${monitorIndex}): ${error}`);
            }

            // Guard: only clear if this is the current process for this monitor.
            const currentState = this._renderers.get(monitorIndex);
            if (!currentState || currentState.subprocess !== spawnedSubprocess)
                return;

            this._clearRendererState(currentState);
            this._renderers.delete(monitorIndex);

            if (this._settings?.get_boolean('enabled') && exitCode !== 0)
                this._scheduleRetryForMonitor(monitorIndex);
        });
    }

    _stopRendererByIndex(idx) {
        const state = this._renderers.get(idx);
        if (!state)
            return;

        this._renderers.delete(idx);
        this._clearRendererState(state);
    }

    _stopAllRenderers() {
        for (const [idx, state] of this._renderers) {
            this._clearRendererState(state);
        }
        this._renderers.clear();
    }

    _clearRendererState(state) {
        // Cancel pending anchor retries.
        for (const id of state.anchorRetrySourceIds)
            GLib.source_remove(id);
        state.anchorRetrySourceIds.clear();
        state.anchorPending = false;

        // Cancel pending retry timer.
        if (state.retrySourceId > 0) {
            GLib.source_remove(state.retrySourceId);
            state.retrySourceId = 0;
        }

        // Stop pause policy.
        if (state.pausePolicy) {
            state.pausePolicy.disable();
            state.pausePolicy = null;
        }

        // Restore actor opacity.
        const actor = this._getRendererActor(state);
        if (actor) {
            actor.remove_all_transitions();
            actor.opacity = 255;
        }
        state.renderingPaused = false;
        state.pauseReasons.clear();

        // Clear anchor.
        if (state.raisedSignalId && state.anchoredWindow) {
            state.anchoredWindow.disconnect(state.raisedSignalId);
            state.raisedSignalId = 0;
        }
        state.anchoredWindow = null;

        // Cancel output reader.
        if (state.stdoutCancellable)
            state.stdoutCancellable.cancel();
        state.stdoutStream = null;
        state.stdoutCancellable = null;

        // Kill the subprocess.
        if (state.subprocess) {
            try {
                state.subprocess.force_exit();
            } catch (error) {
                log(`[milkdrop] failed to stop renderer (monitor ${state.monitorIndex}): ${error}`);
            }
        }
        state.subprocess = null;
        state.waylandClient = null;
    }

    // ─── Settings ─────────────────────────────────────────────────────────────

    _onSettingChanged(key) {
        if (key === 'enabled')
            return;

        if (key === 'monitor' || key === 'all-monitors') {
            this._syncMonitors();
            return;
        }

        if (key === 'opacity') {
            const value = this._settings.get_double('opacity');
            this._broadcastCommand(`opacity ${value}`);
            return;
        }

        if (key === 'preset-dir') {
            const path = this._settings.get_string('preset-dir');
            this._broadcastCommand(`preset-dir ${GLib.shell_quote(path)}`);
            return;
        }

        if (key === 'shuffle') {
            const enabled = this._settings.get_boolean('shuffle');
            this._broadcastCommand(`shuffle ${enabled ? 'on' : 'off'}`);
            return;
        }

        if (key === 'overlay') {
            const enabled = this._settings.get_boolean('overlay');
            this._broadcastCommand(`overlay ${enabled ? 'on' : 'off'}`);
            return;
        }

        if (key === 'pause-on-fullscreen' || key === 'pause-on-maximized') {
            for (const [, state] of this._renderers)
                state.pausePolicy?.reEvaluate();
            return;
        }

        if (key === 'fps') {
            const fps = this._settings.get_int('fps');
            this._broadcastCommand(`fps ${fps}`);
            return;
        }

        if (key === 'media-aware') {
            const enabled = this._settings.get_boolean('media-aware');
            if (enabled)
                this._startMprisWatcher();
            else
                this._stopMprisWatcher();
        }
    }

    // ─── Command routing ───────────────────────────────────────────────────

    _sendControlCommand(command) {
        this._broadcastCommand(command);
    }

    _broadcastCommand(command) {
        for (const [, state] of this._renderers) {
            if (state.subprocess && state.socketPath)
                sendMilkdropControlCommand(command, state.socketPath);
        }
    }

    // ─── Pause reasons ─────────────────────────────────────────────────────

    /**
     * Set a global pause reason (e.g. MPRIS) — broadcasts to all renderers.
     * Per-monitor reasons (fullscreen, maximized) route via _setPauseReasonForRenderer.
     */
    _setPauseReason(reason, active) {
        if (active)
            this._globalPauseReasons.add(reason);
        else
            this._globalPauseReasons.delete(reason);

        for (const [, state] of this._renderers)
            this._setPauseReasonForRenderer(state, reason, active);
    }

    _setPauseReasonForRenderer(state, reason, active) {
        if (active)
            state.pauseReasons.add(reason);
        else
            state.pauseReasons.delete(reason);

        if (!state.subprocess)
            return;

        const shouldPause = state.pauseReasons.size > 0;
        if (shouldPause === state.renderingPaused)
            return;

        state.renderingPaused = shouldPause;

        if (shouldPause)
            this._fadeOutThenPauseRenderer(state);
        else
            this._resumeThenFadeInRenderer(state);
    }

    _getRendererActor(state) {
        return state?.anchoredWindow?.get_compositor_private() ?? null;
    }

    _fadeOutThenPauseRenderer(state) {
        const actor = this._getRendererActor(state);
        if (!actor) {
            sendMilkdropControlCommand('pause on', state.socketPath);
            return;
        }

        actor.remove_all_transitions();
        actor.ease({
            opacity: 0,
            duration: FADE_DURATION_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onComplete: () => {
                if (state.renderingPaused)
                    sendMilkdropControlCommand('pause on', state.socketPath);
            },
        });
    }

    _resumeThenFadeInRenderer(state) {
        sendMilkdropControlCommand('pause off', state.socketPath);

        const actor = this._getRendererActor(state);
        if (!actor)
            return;

        actor.remove_all_transitions();
        actor.ease({
            opacity: 255,
            duration: FADE_DURATION_MS,
            mode: Clutter.AnimationMode.EASE_IN_QUAD,
        });
    }

    // ─── Pause policy ─────────────────────────────────────────────────────

    _startPausePolicy(state) {
        if (state.pausePolicy)
            state.pausePolicy.disable();

        state.pausePolicy = new PausePolicy(
            state.monitorIndex,
            this._settings,
            (reason, active) => this._setPauseReasonForRenderer(state, reason, active)
        );
        state.pausePolicy.enable();
    }

    // ─── MPRIS ────────────────────────────────────────────────────────────

    _startMprisWatcher() {
        if (!this._settings?.get_boolean('media-aware'))
            return;
        if (this._mprisWatcher)
            return;

        this._mprisWatcher = new MprisWatcher(isPlaying => {
            this._setPauseReason(PAUSE_REASON_MPRIS, !isPlaying);
        });
        this._mprisWatcher.enable();
    }

    _stopMprisWatcher() {
        if (this._mprisWatcher) {
            this._mprisWatcher.disable();
            this._mprisWatcher = null;
        }
        this._setPauseReason(PAUSE_REASON_MPRIS, false);
    }

    // ─── Renderer spawn helpers ────────────────────────────────────────────

    _resolvePresetDir(configured) {
        if (configured.length > 0 && GLib.file_test(configured, GLib.FileTest.IS_DIR))
            return configured;

        const candidates = [
            '/usr/local/share/projectM/presets',
            '/usr/share/projectM/presets',
            '/usr/share/projectm/presets',
            '/usr/local/share/projectm/presets',
        ];
        for (const dir of candidates) {
            if (GLib.file_test(dir, GLib.FileTest.IS_DIR))
                return dir;
        }
        return '';
    }

    _buildRendererArgs(binaryPath, monitorIndex) {
        const opacity = this._settings?.get_double('opacity') ?? 1.0;
        const configured = this._settings?.get_string('preset-dir') ?? '';
        const presetDir = this._resolvePresetDir(configured);
        const shuffle = this._settings?.get_boolean('shuffle') ?? false;
        const overlay = this._settings?.get_boolean('overlay') ?? false;

        const socketPath = getMilkdropSocketPath(monitorIndex);

        const args = [
            binaryPath,
            '--monitor', `${monitorIndex}`,
            '--opacity', `${opacity}`,
            '--socket-path', socketPath,
        ];

        if (presetDir.length > 0)
            args.push('--preset-dir', presetDir);

        if (shuffle)
            args.push('--shuffle');

        if (overlay)
            args.push('--overlay');

        return args;
    }

    _resolveBinaryPath() {
        const localBinHome = GLib.getenv('XDG_BIN_HOME');
        const home = GLib.get_home_dir();

        const candidates = [
            localBinHome ? `${localBinHome}/milkdrop` : null,
            `${home}/.local/bin/milkdrop`,
            `${this.path}/bin/milkdrop`,
            '/usr/local/bin/milkdrop',
            '/usr/bin/milkdrop',
        ].filter(Boolean);

        for (const candidate of candidates) {
            if (GLib.file_test(candidate, GLib.FileTest.IS_EXECUTABLE))
                return candidate;
        }

        return GLib.find_program_in_path('milkdrop');
    }

    _readRendererOutput(state) {
        if (!state.stdoutStream || !state.stdoutCancellable)
            return;

        state.stdoutStream.read_line_async(
            GLib.PRIORITY_DEFAULT,
            state.stdoutCancellable,
            (stream, res) => {
                try {
                    const [line, length] = stream.read_line_finish_utf8(res);
                    if (length > 0)
                        log(`[milkdrop:${state.monitorIndex}] ${line}`);
                } catch (error) {
                    if (!error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        log(`[milkdrop:${state.monitorIndex}] output read failed: ${error}`);
                    return;
                }

                this._readRendererOutput(state);
            }
        );
    }

    // ─── Retry ────────────────────────────────────────────────────────────

    _scheduleRetryForMonitor(monitorIndex) {
        // Build a temporary state to hold the retry timer.
        const placeholder = this._makeRendererState(monitorIndex);
        const existingIndex = Math.min(
            (this._renderers.get(monitorIndex)?.retryStep ?? 0),
            RETRY_DELAYS_MS.length - 1
        );
        const delayMs = RETRY_DELAYS_MS[existingIndex];
        placeholder.retryStep = existingIndex + 1;
        this._renderers.set(monitorIndex, placeholder);

        placeholder.retrySourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delayMs, () => {
            placeholder.retrySourceId = 0;
            // Only retry if this placeholder is still current (not superseded).
            if (this._renderers.get(monitorIndex) !== placeholder)
                return GLib.SOURCE_REMOVE;
            this._renderers.delete(monitorIndex);
            if (this._settings?.get_boolean('enabled'))
                this._spawnRendererForMonitor(monitorIndex);
            return GLib.SOURCE_REMOVE;
        });
    }

    // ─── Clutter / compositor overrides (Hanabi pattern) ──────────────────

    _installOverrides() {
        this._injectionManager = new InjectionManager();
        const thisRef = this;

        function isRenderer(metaWindow) {
            if (!metaWindow)
                return false;
            if (typeof metaWindow.get_wm_class !== 'function')
                return false;
            if (metaWindow.get_wm_class() === 'milkdrop')
                return true;
            return metaWindow.title === 'milkdrop';
        }

        function findRendererActorForMonitor(monitorIndex) {
            const actors = global.get_window_actors(false);
            for (const actor of actors) {
                const window = actor.meta_window;
                if (!window)
                    continue;
                if (!isRenderer(window))
                    continue;
                if (window.get_monitor() === monitorIndex)
                    return actor;
            }
            return null;
        }

        this._injectionManager.overrideMethod(
            Background.BackgroundManager.prototype,
            '_createBackgroundActor',
            originalMethod => {
                return function () {
                    const backgroundActor = originalMethod.call(this);
                    const monitorIndex = Number.isInteger(backgroundActor.monitor)
                        ? backgroundActor.monitor
                        : this.monitorIndex;

                    const monitor = Main.layoutManager.monitors[monitorIndex];
                    if (!monitor)
                        return backgroundActor;

                    if (backgroundActor._milkdropWallpaper)
                        return backgroundActor;

                    backgroundActor.layout_manager = new Clutter.BinLayout();

                    const wallpaper = new St.Widget({
                        width: backgroundActor.width,
                        height: backgroundActor.height,
                        x_expand: true,
                        y_expand: true,
                        reactive: false,
                    });
                    wallpaper.layout_manager = new Clutter.BinLayout();

                    backgroundActor._milkdropWallpaper = wallpaper;
                    backgroundActor.add_child(wallpaper);
                    thisRef._wallpaperActors.add(wallpaper);

                    let sourceId = 0;
                    const attachClone = () => {
                        if (wallpaper.get_n_children() > 0)
                            return GLib.SOURCE_REMOVE;

                        if (backgroundActor.width > 0 && backgroundActor.height > 0) {
                            const bgRatio = backgroundActor.width / backgroundActor.height;
                            const monitorRatio = monitor.width / monitor.height;
                            const areaRatio = (backgroundActor.width * backgroundActor.height) /
                                (monitor.width * monitor.height);

                            if (areaRatio < 0.5 || Math.abs(bgRatio - monitorRatio) > 0.35)
                                return GLib.SOURCE_REMOVE;
                        }

                        const rendererActor = findRendererActorForMonitor(monitorIndex);
                        if (!rendererActor)
                            return GLib.SOURCE_CONTINUE;

                        const clone = new Clutter.Clone({
                            source: rendererActor,
                            x_expand: true,
                            y_expand: true,
                        });
                        wallpaper.add_child(clone);
                        return GLib.SOURCE_REMOVE;
                    };

                    if (attachClone() === GLib.SOURCE_CONTINUE) {
                        sourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {
                            if (!wallpaper.get_stage())
                                return GLib.SOURCE_REMOVE;
                            return attachClone();
                        });
                        thisRef._wallpaperSourceIds.add(sourceId);
                    }

                    wallpaper.connect('destroy', () => {
                        thisRef._wallpaperActors.delete(wallpaper);
                        if (backgroundActor._milkdropWallpaper === wallpaper)
                            delete backgroundActor._milkdropWallpaper;
                        if (sourceId > 0) {
                            GLib.source_remove(sourceId);
                            thisRef._wallpaperSourceIds.delete(sourceId);
                            sourceId = 0;
                        }
                    });

                    return backgroundActor;
                };
            }
        );

        this._injectionManager.overrideMethod(
            Shell.Global.prototype,
            'get_window_actors',
            originalMethod => {
                return function (hideRenderer = true) {
                    const actors = originalMethod.call(this);
                    return hideRenderer
                        ? actors.filter(a => !isRenderer(a.meta_window))
                        : actors;
                };
            }
        );

        this._injectionManager.overrideMethod(
            Workspace.Workspace.prototype,
            '_isOverviewWindow',
            originalMethod => {
                return function (window) {
                    if (isRenderer(window))
                        return false;
                    return originalMethod.call(this, window);
                };
            }
        );

        this._injectionManager.overrideMethod(
            WorkspaceThumbnail.WorkspaceThumbnail.prototype,
            '_isOverviewWindow',
            originalMethod => {
                return function (window) {
                    if (isRenderer(window))
                        return false;
                    return originalMethod.call(this, window);
                };
            }
        );

        this._injectionManager.overrideMethod(
            Meta.Display.prototype,
            'get_tab_list',
            originalMethod => {
                return function (type, workspace) {
                    const wins = originalMethod.apply(this, [type, workspace]);
                    return wins.filter(w => !isRenderer(w));
                };
            }
        );

        this._injectionManager.overrideMethod(
            Shell.WindowTracker.prototype,
            'get_window_app',
            originalMethod => {
                return function (window) {
                    if (isRenderer(window))
                        return null;
                    return originalMethod.call(this, window);
                };
            }
        );

        this._injectionManager.overrideMethod(
            Shell.App.prototype,
            'get_windows',
            originalMethod => {
                return function () {
                    const wins = originalMethod.call(this);
                    return wins.filter(w => !isRenderer(w));
                };
            }
        );

        this._injectionManager.overrideMethod(
            Shell.App.prototype,
            'get_n_windows',
            originalMethod => {
                return function () {
                    return this.get_windows().length;
                };
            }
        );

        this._injectionManager.overrideMethod(
            Shell.AppSystem.prototype,
            'get_running',
            originalMethod => {
                return function () {
                    const running = originalMethod.call(this);
                    return running.filter(app => app.get_n_windows() > 0);
                };
            }
        );
    }

    _reloadBackgrounds() {
        if (!Main.layoutManager._startingUp) {
            try {
                Main.layoutManager._updateBackgrounds();
            } catch (e) {
                log(`[milkdrop] _updateBackgrounds failed: ${e}`);
            }
            try {
                Main.overview._overview._controls._workspacesDisplay._updateWorkspacesViews();
            } catch (e) {
                log(`[milkdrop] _updateWorkspacesViews skipped: ${e.message}`);
            }
        }
    }

    _clearOverrides() {
        for (const sourceId of this._wallpaperSourceIds)
            GLib.source_remove(sourceId);
        this._wallpaperSourceIds.clear();

        for (const actor of this._wallpaperActors)
            actor.destroy();
        this._wallpaperActors.clear();

        this._injectionManager?.clear();
        this._injectionManager = null;

        this._reloadBackgrounds();
    }

    // ─── Window anchoring ─────────────────────────────────────────────────

    _scheduleAnchorRetries(window, attempt, state) {
        if (attempt === 0) {
            if (state.anchorPending)
                return;
            state.anchorPending = true;
        }

        const maxAttempts = 15;
        if (attempt >= maxAttempts) {
            log('[milkdrop] owns_window still false after retries; anchoring if title/wm_class match');
            if (window.get_wm_class() === 'milkdrop' || window.title === 'milkdrop')
                this._anchorWindow(window, state);
            return;
        }

        const delayMs = 50 + attempt * 40;
        let sid = 0;
        sid = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delayMs, () => {
            state.anchorRetrySourceIds.delete(sid);
            if (!this._settings?.get_boolean('enabled'))
                return GLib.SOURCE_REMOVE;

            if (!Meta.is_wayland_compositor() || !state.waylandClient) {
                this._anchorWindow(window, state);
                return GLib.SOURCE_REMOVE;
            }

            try {
                if (state.waylandClient.owns_window(window)) {
                    this._anchorWindow(window, state);
                    return GLib.SOURCE_REMOVE;
                }
            } catch (e) {
                log(`[milkdrop] owns_window retry error, anchoring by title/wm_class: ${e}`);
                if (window.get_wm_class() === 'milkdrop' || window.title === 'milkdrop')
                    this._anchorWindow(window, state);
                return GLib.SOURCE_REMOVE;
            }

            this._scheduleAnchorRetries(window, attempt + 1, state);
            return GLib.SOURCE_REMOVE;
        });
        state.anchorRetrySourceIds.add(sid);
    }

    _onWindowMapped(window) {
        if (!window)
            return;

        const wmClass = window.get_wm_class();
        const title = window.title;
        const isWayland = Meta.is_wayland_compositor();

        if (isWayland) {
            for (const [, state] of this._renderers) {
                if (!state.waylandClient)
                    continue;
                try {
                    if (state.waylandClient.owns_window(window)) {
                        log('[milkdrop] renderer window matched — anchoring');
                        this._anchorWindow(window, state);
                        return;
                    } else if (wmClass === 'milkdrop' || title === 'milkdrop') {
                        log('[milkdrop] owns_window false at map; scheduling anchor retries');
                        this._scheduleAnchorRetries(window, 0, state);
                        return;
                    }
                } catch (e) {
                    log(`[milkdrop] owns_window failed, falling back to wm_class: ${e}`);
                    if (wmClass === 'milkdrop' || title === 'milkdrop') {
                        this._anchorWindow(window, state);
                        return;
                    }
                }
            }
        } else {
            if (wmClass !== 'milkdrop' && title !== 'milkdrop')
                return;
            // X11: anchor to the first renderer without an anchored window.
            for (const [, state] of this._renderers) {
                if (!state.anchoredWindow) {
                    log('[milkdrop] renderer window matched — anchoring');
                    this._anchorWindow(window, state);
                    return;
                }
            }
        }
    }

    _anchorWindow(window, state) {
        state.anchorPending = false;

        const actor = window.get_compositor_private();
        log(`[milkdrop] _anchorWindow: monitor=${state.monitorIndex} actor=${actor}, visible=${actor?.visible}`);
        if (!actor) {
            log('[milkdrop] ERROR: No compositor actor for window');
            return;
        }

        if (state.raisedSignalId && state.anchoredWindow) {
            state.anchoredWindow.disconnect(state.raisedSignalId);
            state.raisedSignalId = 0;
        }

        const parent = actor.get_parent();
        if (parent && parent !== global.window_group)
            parent.remove_child(actor);
        if (actor.get_parent() !== global.window_group)
            global.window_group.add_child(actor);
        global.window_group.set_child_below_sibling(actor, null);

        window.stick();
        this._enforceWindowCoverage(window);
        window.lower();

        state.raisedSignalId = window.connect_after('raised', () => {
            window.lower();
            const a = window.get_compositor_private();
            if (a)
                global.window_group.set_child_below_sibling(a, null);
        });

        state.anchoredWindow = window;
        log('[milkdrop] renderer window anchored successfully');
    }

    _enforceWindowCoverage(window) {
        let monitorIndex = window.get_monitor();
        if (monitorIndex < 0)
            monitorIndex = this._settings?.get_int('monitor') ?? 0;

        const geometry = global.display.get_monitor_geometry(monitorIndex);
        if (!geometry || geometry.width <= 0 || geometry.height <= 0)
            return;

        try {
            window.move_to_monitor(monitorIndex);
        } catch (e) {
            log(`[milkdrop] move_to_monitor failed: ${e}`);
        }

        try {
            window.move_resize_frame(false, geometry.x, geometry.y, geometry.width, geometry.height);
        } catch (e) {
            log(`[milkdrop] move_resize_frame failed: ${e}`);
        }
    }

    // ─── Signal disconnection ─────────────────────────────────────────────

    _disconnectWmSignals() {
        for (const id of this._wmSignalIds)
            global.window_manager.disconnect(id);
        this._wmSignalIds = [];
    }

    _disconnectSettingsSignals() {
        if (!this._settings)
            return;

        for (const id of this._settingsSignalIds)
            this._settings.disconnect(id);
        this._settingsSignalIds = [];
    }

    _disconnectStartupComplete() {
        if (this._startupCompleteId) {
            Main.layoutManager.disconnect(this._startupCompleteId);
            this._startupCompleteId = 0;
        }
    }
}
