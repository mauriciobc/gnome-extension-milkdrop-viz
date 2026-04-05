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

        this._launcher = null;
        this._waylandClient = null;
        this._subprocess = null;
        this._stdoutStream = null;
        this._stdoutCancellable = null;

        this._retrySourceId = 0;
        this._retryStep = 0;

        this._startupCompleteId = 0;

        this._anchoredWindow = null;
        this._raisedSignalId = 0;
        this._injectionManager = null;
        this._wallpaperActors = new Set();
        this._wallpaperSourceIds = new Set();
        this._overviewShowingId = 0;
        this._overviewHiddenId = 0;
        this._workareasChangedId = 0;
        this._monitorsChangedId = 0;

        this._anchorRetrySourceIds = new Set();
        this._anchorPending = false;

        this._pauseReasons = new Set();
        this._pausePolicy = null;
        this._mprisWatcher = null;
        this._renderingPaused = false;
    }

    enable() {
        this._settings = this.getSettings();

        this._settingsSignalIds.push(
            this._settings.connect('changed::enabled', () => this._syncEnabledState()),
            this._settings.connect('changed', (_settings, key) => this._onSettingChanged(key))
        );

        // Use the window_manager `map` signal (Hanabi pattern): fires after the
        // Wayland surface has its app_id committed, unlike `window-created` which
        // fires too early for wm_class to be set.
        this._wmSignalIds.push(
            global.window_manager.connect_after('map', (_wm, windowActor) => {
                const window = windowActor.get_meta_window();
                this._onWindowMapped(window);
            })
        );

        this._installOverrides();
        // Force background actors to be recreated with our override immediately
        // so the renderer clone is present in the overview from the first open
        // (Hanabi pattern — without this, only backgrounds created after enable()
        // get the clone, leaving the overview uncovered until a monitor event fires).
        this._reloadBackgrounds();

        // Overview signal handlers: hide the actual renderer window actor during
        // overview transitions so only the BackgroundManager clone is visible
        // (prevents double-image overlap between window_group actor and clone).
        this._overviewShowingId = Main.overview.connect('showing', () => {
            const actor = this._anchoredWindow?.get_compositor_private();
            if (actor)
                actor.hide();
        });
        this._overviewHiddenId = Main.overview.connect('hidden', () => {
            const actor = this._anchoredWindow?.get_compositor_private();
            if (actor)
                actor.show();
            if (this._anchoredWindow)
                this._enforceWindowCoverage(this._anchoredWindow);
        });

        // Keep coverage correct when shell layout changes after startup.
        this._workareasChangedId = global.display.connect('workareas-changed', () => {
            if (this._anchoredWindow)
                this._enforceWindowCoverage(this._anchoredWindow);
        });
        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed', () => {
            if (this._anchoredWindow)
                this._enforceWindowCoverage(this._anchoredWindow);
            this._reloadBackgrounds();
        });

        // Defer renderer spawn until the shell startup animation completes.
        // This prevents a race where the renderer window appears before dock/panel
        // extensions (e.g. dash2dock-lite) have initialized, which can cause them
        // to treat the renderer as a fullscreen window and hide themselves permanently
        // (Hanabi pattern).
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
        this._removeRetrySource();
        this._clearAnchorRetrySources();
        this._disconnectWmSignals();
        this._disconnectSettingsSignals();
        this._stopProcess();
        this._clearAnchor();
        this._clearOverrides();
        this._settings = null;
    }

    _syncEnabledState() {
        if (!this._settings)
            return;

        if (this._settings.get_boolean('enabled'))
            this._spawnProcess();
        else
            this._stopProcess();
    }

    _onSettingChanged(key) {
        if (key === 'enabled')
            return;

        if (!this._subprocess)
            return;

        if (key === 'monitor') {
            this._restartProcess();
            return;
        }

        if (key === 'opacity') {
            const value = this._settings.get_double('opacity');
            this._sendControlCommand(`opacity ${value}`);
            return;
        }

        if (key === 'preset-dir') {
            const path = this._settings.get_string('preset-dir');
            this._sendControlCommand(`preset-dir ${GLib.shell_quote(path)}`);
            return;
        }

        if (key === 'shuffle') {
            const enabled = this._settings.get_boolean('shuffle');
            this._sendControlCommand(`shuffle ${enabled ? 'on' : 'off'}`);
            return;
        }

        if (key === 'overlay') {
            const enabled = this._settings.get_boolean('overlay');
            this._sendControlCommand(`overlay ${enabled ? 'on' : 'off'}`);
            return;
        }

        if (key === 'pause-on-fullscreen' || key === 'pause-on-maximized') {
            this._pausePolicy?.reEvaluate();
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

    _getSocketPath() {
        return getMilkdropSocketPath();
    }

    _sendControlCommand(command) {
        sendMilkdropControlCommand(command);
    }

    /**
     * Coordinate multiple pause reasons into a single animated transition.
     * The renderer stays paused as long as any reason is active.
     * Deduplicates: only acts when the desired state actually changes.
     */
    _setPauseReason(reason, active) {
        if (active)
            this._pauseReasons.add(reason);
        else
            this._pauseReasons.delete(reason);

        if (!this._subprocess)
            return;

        const shouldPause = this._pauseReasons.size > 0;
        if (shouldPause === this._renderingPaused)
            return;

        this._renderingPaused = shouldPause;

        if (shouldPause)
            this._fadeOutThenPause();
        else
            this._resumeThenFadeIn();
    }

    _getRendererActor() {
        return this._anchoredWindow?.get_compositor_private() ?? null;
    }

    /**
     * Fade the renderer actor to transparent, then send `pause on`.
     * If a fade-in is in progress it is cancelled at its current opacity
     * and the direction reverses.
     */
    _fadeOutThenPause() {
        const actor = this._getRendererActor();
        if (!actor) {
            this._sendControlCommand('pause on');
            return;
        }

        actor.remove_all_transitions();
        actor.ease({
            opacity: 0,
            duration: FADE_DURATION_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onComplete: () => {
                // Guard: a rapid resume may have already cleared the pause reasons.
                if (this._renderingPaused)
                    this._sendControlCommand('pause on');
            },
        });
    }

    /**
     * Send `pause off` immediately so the renderer starts producing frames,
     * then fade the actor from its current opacity back to fully opaque.
     */
    _resumeThenFadeIn() {
        this._sendControlCommand('pause off');

        const actor = this._getRendererActor();
        if (!actor)
            return;

        actor.remove_all_transitions();
        actor.ease({
            opacity: 255,
            duration: FADE_DURATION_MS,
            mode: Clutter.AnimationMode.EASE_IN_QUAD,
        });
    }

    _startPausePolicy() {
        if (this._pausePolicy)
            this._pausePolicy.disable();

        const monitorIndex = this._settings?.get_int('monitor') ?? 0;
        this._pausePolicy = new PausePolicy(
            monitorIndex,
            this._settings,
            (reason, active) => this._setPauseReason(reason, active)
        );
        this._pausePolicy.enable();
    }

    _stopPausePolicy() {
        if (this._pausePolicy) {
            this._pausePolicy.disable();
            this._pausePolicy = null;
        }
        this._pauseReasons.clear();
    }

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
        // Clear the MPRIS pause reason so the renderer is not left paused.
        this._setPauseReason(PAUSE_REASON_MPRIS, false);
    }

    _spawnProcess() {
        if (this._subprocess)
            return;

        const binaryPath = this._resolveBinaryPath();
        if (!binaryPath) {
            log(`[milkdrop] renderer binary not found`);
            return;
        }

        const argv = this._buildRendererArgs(binaryPath);
        const shellMajor = parseInt(Config.PACKAGE_VERSION.split('.')[0], 10);
        const isWayland = Meta.is_wayland_compositor();

        this._launcher = new Gio.SubprocessLauncher({
            flags: Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE,
        });
        /* F3a: force GTK4's OpenGL renderer to avoid Mesa's vkps rw-semaphore
         * deadlock that causes the renderer to enter an uninterruptible D-state
         * when the Vulkan presentation path races with the DRM kernel writer. */
        this._launcher.setenv('GSK_RENDERER', 'gl', true);
        /* GtkGLArea pode escolher GLES; projectM precisa de OpenGL desktop (GLSL 330). */
        this._launcher.setenv('MILKDROP_FORCE_GL_API', '1', true);

        try {
            if (isWayland) {
                if (shellMajor < 49) {
                    this._waylandClient = Meta.WaylandClient.new(global.context, this._launcher);
                    this._subprocess = this._waylandClient.spawnv(global.display, argv);
                } else {
                    this._waylandClient = Meta.WaylandClient.new_subprocess(global.context, this._launcher, argv);
                    if (!this._waylandClient) {
                        log('[milkdrop] Meta.WaylandClient.new_subprocess returned null');
                        this._subprocess = null;
                    } else {
                        this._subprocess = this._waylandClient.get_subprocess();
                    }
                }
            } else {
                this._subprocess = this._launcher.spawnv(argv);
            }
        } catch (error) {
            const msg = error?.message ?? String(error);
            log(`[milkdrop] failed to spawn renderer: ${msg}`);
            this._subprocess = null;
            this._waylandClient = null;
        }

        this._launcher = null;

        if (!this._subprocess) {
            this._scheduleRetry();
            return;
        }

        this._retryStep = 0;
        this._startPausePolicy();
        this._startMprisWatcher();
        this._stdoutCancellable = new Gio.Cancellable();
        this._stdoutStream = Gio.DataInputStream.new(this._subprocess.get_stdout_pipe());
        this._readRendererOutput();

        const spawnedSubprocess = this._subprocess;
        const spawnedCancellable = this._stdoutCancellable;
        spawnedSubprocess.wait_async(spawnedCancellable, (proc, res) => {
            let exitCode = -1;
            try {
                proc.wait_finish(res);
                exitCode = proc.get_exit_status();
            } catch (error) {
                if (!error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                    log(`[milkdrop] renderer wait failed: ${error}`);
            }

            /* Guard: only clear state if this callback belongs to the current
             * subprocess. A stale wait_async from a previously killed renderer
             * must not wipe out a newly spawned replacement (enable/disable cycle). */
            if (this._subprocess !== spawnedSubprocess)
                return;

            this._stdoutStream = null;
            this._stdoutCancellable = null;
            this._subprocess = null;
            this._waylandClient = null;
            this._clearAnchor();

            if (this._settings?.get_boolean('enabled') && exitCode !== 0)
                this._scheduleRetry();
        });
    }

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

    _buildRendererArgs(binaryPath) {
        const monitor = this._settings?.get_int('monitor') ?? 0;
        const opacity = this._settings?.get_double('opacity') ?? 1.0;
        const configured = this._settings?.get_string('preset-dir') ?? '';
        const presetDir = this._resolvePresetDir(configured);
        const shuffle = this._settings?.get_boolean('shuffle') ?? false;
        const overlay = this._settings?.get_boolean('overlay') ?? false;

        const args = [
            binaryPath,
            '--monitor', `${monitor}`,
            '--opacity', `${opacity}`,
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

    _readRendererOutput() {
        if (!this._stdoutStream || !this._stdoutCancellable)
            return;

        this._stdoutStream.read_line_async(
            GLib.PRIORITY_DEFAULT,
            this._stdoutCancellable,
            (stream, res) => {
                try {
                    const [line, length] = stream.read_line_finish_utf8(res);
                    if (length > 0)
                        log(`[milkdrop] ${line}`);
                } catch (error) {
                    if (!error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        log(`[milkdrop] output read failed: ${error}`);
                    return;
                }

                this._readRendererOutput();
            }
        );
    }

    _stopProcess() {
        this._removeRetrySource();
        this._stopPausePolicy();
        this._stopMprisWatcher();
        // Abort any in-progress fade and restore full opacity for the next spawn.
        const actor = this._getRendererActor();
        if (actor) {
            actor.remove_all_transitions();
            actor.opacity = 255;
        }
        this._renderingPaused = false;

        if (this._stdoutCancellable)
            this._stdoutCancellable.cancel();

        if (this._subprocess) {
            try {
                this._subprocess.force_exit();
            } catch (error) {
                log(`[milkdrop] failed to stop renderer: ${error}`);
            }
        }

        this._subprocess = null;
        this._waylandClient = null;
        this._stdoutStream = null;
        this._stdoutCancellable = null;
    }

    _restartProcess() {
        this._stopProcess();
        if (this._settings?.get_boolean('enabled'))
            this._spawnProcess();
    }

    _scheduleRetry() {
        this._removeRetrySource();
        const index = Math.min(this._retryStep, RETRY_DELAYS_MS.length - 1);
        const delayMs = RETRY_DELAYS_MS[index];
        this._retryStep++;

        this._retrySourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delayMs, () => {
            this._retrySourceId = 0;
            if (this._settings?.get_boolean('enabled'))
                this._spawnProcess();
            return GLib.SOURCE_REMOVE;
        });
    }

    _removeRetrySource() {
        if (this._retrySourceId > 0) {
            GLib.source_remove(this._retrySourceId);
            this._retrySourceId = 0;
        }
    }

    _installOverrides() {
        this._injectionManager = new InjectionManager();
        const thisRef = this;

        // Identify the renderer by its window title (Hanabi pattern — more
        // reliable than reference comparison; the renderer explicitly sets
        // gtk_window_set_title(window, "milkdrop") in C source).
        function isRenderer(metaWindow) {
            if (!metaWindow)
                return false;
            // Guard: some callers pass non-MetaWindow objects (e.g., WorkspaceThumbnail).
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

        // Hanabi-style overview path: inject a clone of the renderer actor into
        // background actors so the renderer appears as the overview wallpaper
        // while window previews exclude the renderer window card.
        // The actual renderer window actor is hidden during overview via the
        // 'showing'/'hidden' signals above; only the clone is visible in overview.
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

                    // Guard: only add one wallpaper widget per backgroundActor.
                    if (backgroundActor._milkdropWallpaper)
                        return backgroundActor;

                    // Set BinLayout so our actor fills the background area correctly.
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
                        // Only attach if not already done.
                        if (wallpaper.get_n_children() > 0)
                            return GLib.SOURCE_REMOVE;

                        // Only inject into monitor-like backgrounds. This avoids
                        // polluting narrow shell surfaces (e.g. app-grid strips)
                        // with a stretched renderer clone.
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

        // Hide from get_window_actors() so dock autohide logic never sees the
        // renderer as a fullscreen window covering the screen.
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

        // Hide the renderer's window card from the overview workspace grid.
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

        // Hide the renderer's window card from workspace thumbnails.
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

        // Remove from Alt+Tab / Ctrl+Alt+Tab.
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

        // Ensure the renderer is not associated with any app entry in the dash.
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
            _originalMethod => {
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
        // Hanabi pattern: force all BackgroundManager instances to recreate
        // their background actors so our _createBackgroundActor override takes
        // effect for actors created before the extension was enabled.
        // Wrapped in try/catch since the overview tree may not be ready yet.
        if (!Main.layoutManager._startingUp) {
            try {
                Main.layoutManager._updateBackgrounds();
            } catch (e) {
                log(`[milkdrop] _updateBackgrounds failed: ${e}`);
            }
            try {
                Main.overview._overview._controls._workspacesDisplay._updateWorkspacesViews();
            } catch (e) {
                // Not fatal — overview tree might not exist in all shell modes.
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

        // Reload backgrounds to remove our clone actors from all background contexts.
        this._reloadBackgrounds();
    }

    _clearAnchorRetrySources() {
        for (const id of this._anchorRetrySourceIds)
            GLib.source_remove(id);
        this._anchorRetrySourceIds.clear();
    }

    /**
     * On Wayland, owns_window() can be false on the first 'map' even for our
     * subprocess. If we return early, we never move_resize_frame() to the
     * monitor — the renderer stays at GTK default size/position (often invisible
     * or a wrong monitor). Retry a few times, then fall back to wm_class/title.
     */
    _scheduleAnchorRetries(window, attempt) {
        if (attempt === 0) {
            if (this._anchorPending)
                return;
            this._anchorPending = true;
        }

        const maxAttempts = 15;
        if (attempt >= maxAttempts) {
            log('[milkdrop] owns_window still false after retries; anchoring if title/wm_class match');
            if (window.get_wm_class() === 'milkdrop' || window.title === 'milkdrop')
                this._anchorWindow(window);
            return;
        }

        const delayMs = 50 + attempt * 40;
        let sid = 0;
        sid = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delayMs, () => {
            this._anchorRetrySourceIds.delete(sid);
            if (!this._settings?.get_boolean('enabled'))
                return GLib.SOURCE_REMOVE;

            if (!Meta.is_wayland_compositor() || !this._waylandClient) {
                this._anchorWindow(window);
                return GLib.SOURCE_REMOVE;
            }

            try {
                if (this._waylandClient.owns_window(window)) {
                    this._anchorWindow(window);
                    return GLib.SOURCE_REMOVE;
                }
            } catch (e) {
                log(`[milkdrop] owns_window retry error, anchoring by title/wm_class: ${e}`);
                if (window.get_wm_class() === 'milkdrop' || window.title === 'milkdrop')
                    this._anchorWindow(window);
                return GLib.SOURCE_REMOVE;
            }

            this._scheduleAnchorRetries(window, attempt + 1);
            return GLib.SOURCE_REMOVE;
        });
        this._anchorRetrySourceIds.add(sid);
    }

    _onWindowMapped(window) {
        if (!window)
            return;

        const wmClass = window.get_wm_class();
        const title = window.title;
        const isWayland = Meta.is_wayland_compositor();

        // On Wayland, use Wayland client ownership as the primary identification
        // method (Hanabi pattern). This is reliable even when wm_class is not yet
        // set at window-created time.
        if (isWayland && this._waylandClient) {
            try {
                const owns = this._waylandClient.owns_window(window);
                if (!owns) {
                    // Only queue retries if the window could plausibly be ours.
                    // owns_window can transiently return false for our window just
                    // after mapping; for all other windows it will never become true.
                    if (wmClass === 'milkdrop' || title === 'milkdrop') {
                        log('[milkdrop] owns_window false at map; scheduling anchor retries');
                        this._scheduleAnchorRetries(window, 0);
                    }
                    return;
                }
            } catch (error) {
                // Ownership API unavailable — fall through to wm_class check.
                log(`[milkdrop] owns_window failed, falling back to wm_class: ${error}`);
                if (wmClass !== 'milkdrop' && title !== 'milkdrop')
                    return;
            }
        } else {
            // X11 or no waylandClient yet: match by wm_class or title.
            if (wmClass !== 'milkdrop' && title !== 'milkdrop')
                return;
        }

        log('[milkdrop] renderer window matched — anchoring');
        this._anchorWindow(window);
    }

    _anchorWindow(window) {
        this._anchorPending = false;

        const actor = window.get_compositor_private();
        log(`[milkdrop] _anchorWindow: actor=${actor}, visible=${actor?.visible}`);
        if (!actor) {
            log('[milkdrop] ERROR: No compositor actor for window');
            return;
        }

        // Disconnect any previous raised-signal before re-anchoring.
        if (this._raisedSignalId && this._anchoredWindow) {
            this._anchoredWindow.disconnect(this._raisedSignalId);
            this._raisedSignalId = 0;
        }

        // Clutter layer: move actor to the bottom of the window group.
        const parent = actor.get_parent();
        log(`[milkdrop] Actor parent: ${parent}, window_group: ${global.window_group}`);
        if (parent && parent !== global.window_group)
            parent.remove_child(actor);
        if (actor.get_parent() !== global.window_group)
            global.window_group.add_child(actor);
        global.window_group.set_child_below_sibling(actor, null);
        log('[milkdrop] Actor moved to bottom of window_group');

        // Stick to all workspaces so the wallpaper is visible everywhere.
        window.stick();
        log('[milkdrop] Window stuck to all workspaces');

        // Enforce monitor coverage so the renderer behaves like a wallpaper
        // surface instead of a regular centered toplevel.
        this._enforceWindowCoverage(window);

        // Meta layer: lower the window in Mutter's window stack so it cannot
        // receive input focus or block other windows (Hanabi pattern).
        window.lower();
        log('[milkdrop] Window lowered in Meta stack');

        // Re-enforce keep-at-bottom whenever Mutter tries to raise the window.
        this._raisedSignalId = window.connect_after('raised', () => {
            window.lower();
            const a = window.get_compositor_private();
            if (a)
                global.window_group.set_child_below_sibling(a, null);
        });

        this._anchoredWindow = window;
        log('[milkdrop] renderer window anchored successfully');
    }

    _enforceWindowCoverage(window) {
        let monitorIndex = window.get_monitor();
        if (monitorIndex < 0)
            monitorIndex = this._settings?.get_int('monitor') ?? 0;
        log(`[milkdrop] _enforceWindowCoverage: monitor=${monitorIndex}`);

        const geometry = global.display.get_monitor_geometry(monitorIndex);
        log(`[milkdrop] Monitor geometry: ${geometry?.x},${geometry?.y} ${geometry?.width}x${geometry?.height}`);
        if (!geometry || geometry.width <= 0 || geometry.height <= 0) {
            log('[milkdrop] ERROR: Invalid monitor geometry');
            return;
        }

        // Mutter-level request for the toplevel bounds.
        try {
            window.move_to_monitor(monitorIndex);
            log(`[milkdrop] move_to_monitor(${monitorIndex}) succeeded`);
        } catch (e) {
            log(`[milkdrop] move_to_monitor failed: ${e}`);
        }

        try {
            window.move_resize_frame(
                false,
                geometry.x,
                geometry.y,
                geometry.width,
                geometry.height
            );
            log(`[milkdrop] move_resize_frame to ${geometry.width}x${geometry.height} succeeded`);
        } catch (e) {
            log(`[milkdrop] move_resize_frame failed: ${e}`);
        }
    }

    _clearAnchor() {
        this._anchorPending = false;
        this._clearAnchorRetrySources();
        if (this._raisedSignalId && this._anchoredWindow) {
            this._anchoredWindow.disconnect(this._raisedSignalId);
            this._raisedSignalId = 0;
        }
        this._anchoredWindow = null;
    }

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