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

import {RETRY_DELAYS_MS, RELOAD_BACKGROUNDS_DEBOUNCE_MS, CONTROL_SOCKET_RETRY_DELAY_MS, CONTROL_SOCKET_MAX_RETRIES} from './constants.js';
import {getMilkdropSocketPath, sendMilkdropControlCommand, sendMilkdropControlCommandsSequentially} from './controlClient.js';
import {ManagedWindow} from './managedWindow.js';
import {PausePolicy} from './pausePolicy.js';
import {MprisWatcher} from './mprisWatcher.js';

/**
 * Safely detect if running on Wayland.
 * _isWayland() may not be available in all GNOME Shell versions.
 */
function _isWayland() {
    // Check if the Meta function exists and is callable
    if (typeof Meta.is_wayland_compositor === 'function')
        return Meta.is_wayland_compositor();
    // Fallback: check session type via environment
    const sessionType = GLib.getenv('XDG_SESSION_TYPE');
    return sessionType === 'wayland';
}

/* Map GSettings keys to control-socket command strings.
 * Each value is a function(settings) → command string. */
const SETTING_DISPATCH = {
    'opacity':  s => `opacity ${s.get_double('opacity')}`,
    'shuffle':  s => `shuffle ${s.get_boolean('shuffle') ? 'on' : 'off'}`,
    'fps':      s => `fps ${s.get_int('fps')}`,
    'overlay':  s => `overlay ${s.get_boolean('overlay') ? 'on' : 'off'}`,
    'preset-dir': s => `preset-dir ${GLib.shell_quote(s.get_string('preset-dir'))}`,
    'preset-rotation-interval': s => `rotation-interval ${s.get_int('preset-rotation-interval')}`,
    'beat-sensitivity': s => `beat-sensitivity ${s.get_double('beat-sensitivity')}`,
    'hard-cut-enabled': s => `hard-cut-enabled ${s.get_boolean('hard-cut-enabled') ? 'on' : 'off'}`,
    'hard-cut-sensitivity': s => `hard-cut-sensitivity ${s.get_double('hard-cut-sensitivity')}`,
    'hard-cut-duration': s => `hard-cut-duration ${s.get_double('hard-cut-duration')}`,
    'soft-cut-duration': s => `soft-cut-duration ${s.get_double('soft-cut-duration')}`,
};

export default class MilkdropExtension extends Extension {
    constructor(metadata) {
        super(metadata);

        this._settings = null;
        this._settingsSignalIds = [];
        this._wmSignalIds = [];
        this._windowCreatedId = 0;

        // Multi-process support: Maps indexed by monitor index
        this._launchers = new Map();           // monitorIndex -> Gio.SubprocessLauncher
        this._waylandClients = new Map();      // monitorIndex -> Meta.WaylandClient
        this._subprocesses = new Map();        // monitorIndex -> Gio.Subprocess
        this._stdoutStreams = new Map();     // monitorIndex -> Gio.DataInputStream
        this._stdoutCancellables = new Map();  // monitorIndex -> Gio.Cancellable
        this._retrySourceIds = new Map();      // monitorIndex -> sourceId
        this._retrySteps = new Map();          // monitorIndex -> number

        this._startupCompleteId = 0;

        // Track managed windows per monitor (ManagedWindow instances)
        this._managedWindows = new Map();      // monitorIndex -> ManagedWindow
        this._injectionManager = null;
        this._wallpaperActors = new Set();
        this._wallpaperSourceIds = new Set();
        this._overviewShowingId = 0;
        this._overviewHiddenId = 0;
        this._workareasChangedId = 0;
        this._monitorsChangedId = 0;

        this._anchorRetrySourceIds = new Set();
        this._anchorPending = new Map();      // monitorIndex -> boolean

        // Circuit breaker: track consecutive abnormal renderer deaths
        this._consecutiveAbnormalDeaths = 0;
        this._lastDeathTimestamp = 0;
        this._circuitBreakerOpen = false;

        // State tracking for actor lifecycle management
        this._actorReparentState = new Map();
        this._isDisabling = false;
        this._isEnabling = false;
        this._pendingCleanup = false;

        // Pause coordination state
        this._pauseReasons = {
            fullscreen: false,
            maximized: false,
            mpris: false
        };
        this._pausePolicies = new Map();      // monitorIndex -> PausePolicy
        this._mprisWatcher = null;

        // Debounce source for background reloads
        this._reloadBackgroundsSourceId = 0;
    }

    enable() {
        if (this._isDisabling) {
            log('[milkdrop] Cannot enable during disable transition');
            return;
        }
        // Idempotent: re-entering during deferred startup is safe.
        this._isEnabling = true;

        // Reset circuit breaker on fresh enable
        this._circuitBreakerOpen = false;
        this._consecutiveAbnormalDeaths = 0;

        this._settings = this.getSettings();

        this._connectSettingsSignals();
        this._connectWmSignals();
        this._setupBackgroundOverrides();
        this._connectLayoutSignals();
        this._setupPauseCoordination();
        this._deferSyncUntilStartupComplete();
    }

    _connectSettingsSignals() {
        this._settingsSignalIds.push(
            this._settings.connect('changed::enabled', () => this._syncEnabledState()),
            this._settings.connect('changed', (_settings, key) => this._onSettingChanged(key))
        );
    }

    _connectWmSignals() {
        // Use the window_manager `map` signal (Hanabi pattern): fires after the
        // Wayland surface has its app_id committed, unlike `window-created` which
        // fires too early for wm_class to be set.
        this._wmSignalIds.push(
            global.window_manager.connect_after('map', (_wm, windowActor) => {
                const window = windowActor.get_meta_window();
                this._onWindowMapped(window);
            })
        );

        // Set DESKTOP window type as early as possible — on window-created,
        // not on map.  Mutter starts sending toplevel configure events as
        // soon as the Meta.Window exists (type=NORMAL).  Each configure event
        // causes GTK4 to resize the widget, which triggers
        // milkdrop_maybe_resize_picture → offscreen_renderer_create_target →
        // FBO_INCOMPLETE_ATTACHMENT transiently.  Setting DESKTOP on
        // window-created suppresses those configure events before they start.
        const shellMajor = parseInt(Config.PACKAGE_VERSION.split('.')[0], 10);
        if (shellMajor >= 49 && _isWayland()) {
            this._windowCreatedId = global.display.connect(
                'window-created',
                (_display, window) => {
                    this._onWindowCreated(window);
                }
            );
        }
    }

    _setupBackgroundOverrides() {
        this._installOverrides();
        // Force background actors to be recreated with our override immediately
        // so the renderer clone is present in the overview from the first open
        // (Hanabi pattern — without this, only backgrounds created after enable()
        // get the clone, leaving the overview uncovered until a monitor event fires).
        this._reloadBackgrounds();
    }

    _connectLayoutSignals() {
        // Keep coverage correct when shell layout changes after startup.
        this._workareasChangedId = global.display.connect('workareas-changed', () => {
            for (const managed of this._managedWindows.values())
                this._enforceWindowCoverage(managed.window);
        });
        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed', () => {
            for (const managed of this._managedWindows.values())
                this._enforceWindowCoverage(managed.window);
            this._scheduleReloadBackgrounds();
        });
    }

    _deferSyncUntilStartupComplete() {
        // Defer renderer spawn until the shell startup animation completes.
        // This prevents a race where the renderer window appears before dock/panel
        // extensions (e.g. dash2dock-lite) have initialized, which can cause them
        // to treat the renderer as a fullscreen window and hide themselves permanently
        // (Hanabi pattern).
        if (Main.layoutManager._startingUp) {
            this._startupCompleteId = Main.layoutManager.connect('startup-complete', () => {
                this._disconnectStartupComplete();
                this._syncEnabledState();
                this._isEnabling = false;
            });
        } else {
            this._syncEnabledState();
            this._isEnabling = false;
        }
    }

    _broadcastControlCommand(command) {
        for (const monitorIndex of this._subprocesses.keys())
            this._sendControlCommand(command, monitorIndex);
    }

    /**
     * Coordinates pause reasons and sends pause command when aggregate state changes.
     * @param {string} reason - 'fullscreen', 'maximized', or 'mpris'
     * @param {boolean} active - whether this reason should pause
     */
    _setPauseReason(reason, active) {
        if (this._pauseReasons[reason] === active)
            return;

        this._pauseReasons[reason] = active;

        // Aggregate: pause if ANY reason is active
        const shouldPause = this._pauseReasons.fullscreen ||
                           this._pauseReasons.maximized ||
                           this._pauseReasons.mpris;

        this._broadcastControlCommand(`pause ${shouldPause ? 'on' : 'off'}`);

        log(`[milkdrop] pause state: ${shouldPause ? 'on' : 'off'} (fullscreen:${this._pauseReasons.fullscreen}, maximized:${this._pauseReasons.maximized}, mpris:${this._pauseReasons.mpris})`);
    }

    /**
     * Setup PausePolicy for each monitor and MprisWatcher if enabled.
     */
    _setupPauseCoordination() {
        const pauseOnFullscreen = this._settings.get_boolean('pause-on-fullscreen');
        const pauseOnMaximized = this._settings.get_boolean('pause-on-maximized');
        const mediaAware = this._settings.get_boolean('media-aware');

        // Setup MprisWatcher if media-aware is enabled
        if (mediaAware) {
            this._mprisWatcher = new MprisWatcher(isPlaying => {
                this._setPauseReason('mpris', !isPlaying);
            });
            this._mprisWatcher.enable();
        }

        // Listen for settings changes to pause coordination
        this._settingsSignalIds.push(
            this._settings.connect('changed::pause-on-fullscreen', () => {
                if (!this._settings.get_boolean('pause-on-fullscreen')) {
                    // If disabled, clear the reason
                    this._setPauseReason('fullscreen', false);
                } else {
                    // Re-evaluate existing policies
                    for (const policy of this._pausePolicies.values())
                        policy.reEvaluate();
                }
            }),
            this._settings.connect('changed::pause-on-maximized', () => {
                if (!this._settings.get_boolean('pause-on-maximized')) {
                    this._setPauseReason('maximized', false);
                } else {
                    for (const policy of this._pausePolicies.values())
                        policy.reEvaluate();
                }
            }),
            this._settings.connect('changed::media-aware', () => {
                const enabled = this._settings.get_boolean('media-aware');
                if (enabled && !this._mprisWatcher) {
                    this._mprisWatcher = new MprisWatcher(isPlaying => {
                        this._setPauseReason('mpris', !isPlaying);
                    });
                    this._mprisWatcher.enable();
                } else if (!enabled && this._mprisWatcher) {
                    this._mprisWatcher.disable();
                    this._mprisWatcher = null;
                    this._setPauseReason('mpris', false);
                }
            })
        );
    }

    disable() {
        if (this._isDisabling) {
            log('[milkdrop] Already in transition state, skipping disable');
            return;
        }
        this._isDisabling = true;

        this._disconnectStartupComplete();
        // Overview handlers removed - actor is now directly in wallpaper layer
        if (this._workareasChangedId) {
            global.display.disconnect(this._workareasChangedId);
            this._workareasChangedId = 0;
        }
        if (this._monitorsChangedId) {
            Main.layoutManager.disconnect(this._monitorsChangedId);
            this._monitorsChangedId = 0;
        }
        // Clear debounced background reload if pending
        if (this._reloadBackgroundsSourceId > 0) {
            this._safeRemoveSource(this._reloadBackgroundsSourceId);
            this._reloadBackgroundsSourceId = 0;
        }
        this._removeAllRetrySources();
        this._clearAnchorRetrySources();
        this._disconnectWmSignals();
        this._disconnectSettingsSignals();

        // Stop all renderer processes
        for (const monitorIndex of Array.from(this._subprocesses.keys()))
            this._stopProcess(monitorIndex);

        // Disable all PausePolicies
        for (const policy of this._pausePolicies.values())
            policy.disable();
        this._pausePolicies.clear();

        // Disable MprisWatcher
        if (this._mprisWatcher) {
            this._mprisWatcher.disable();
            this._mprisWatcher = null;
        }

        this._clearAllAnchors();
        this._clearOverrides();
        this._settings = null;
        this._actorReparentState.clear();
        this._isDisabling = false;
        this._isEnabling = false;
    }

    _syncEnabledState() {
        if (!this._settings)
            return;

        const enabled = this._settings.get_boolean('enabled');
        const allMonitors = this._settings.get_boolean('all-monitors');

        if (!enabled) {
            // Stop all renderers
            for (const monitorIndex of Array.from(this._subprocesses.keys()))
                this._stopProcess(monitorIndex);
            return;
        }

        const nMonitors = Main.layoutManager.monitors.length;

        if (allMonitors) {
            // Spawn for all monitors
            for (let i = 0; i < nMonitors; i++)
                this._spawnProcess(i);
        } else {
            const targetMonitor = this._settings.get_int('monitor');

            // Stop monitors that are not the target
            for (const monitorIndex of this._subprocesses.keys()) {
                if (monitorIndex !== targetMonitor)
                    this._stopProcess(monitorIndex);
            }

            // Spawn for target monitor
            this._spawnProcess(targetMonitor);
        }
    }

    _onSettingChanged(key) {
        if (key === 'enabled' || this._subprocesses.size === 0)
            return;

        if (key === 'monitor') {
            if (!this._settings.get_boolean('all-monitors'))
                this._syncEnabledState();
            return;
        }

        if (key === 'all-monitors') {
            this._syncEnabledState();
            return;
        }

        const cmd = SETTING_DISPATCH[key];
        if (cmd)
            this._broadcastControlCommand(cmd(this._settings));
    }

    _sendControlCommand(command, monitorIndex = 0) {
        sendMilkdropControlCommand(command, getMilkdropSocketPath(monitorIndex));
    }

    _spawnProcess(monitorIndex = 0) {
        log(`[milkdrop] _spawnProcess called for monitor ${monitorIndex}`);
        if (this._circuitBreakerOpen) {
            log(`[milkdrop] _spawnProcess: circuit breaker is open, skipping spawn for monitor ${monitorIndex}`);
            return;
        }
        if (this._subprocesses.has(monitorIndex)) {
            log(`[milkdrop] _spawnProcess: subprocess already exists for monitor ${monitorIndex}, returning`);
            return;
        }

        const binaryPath = this._resolveBinaryPath();
        log(`[milkdrop] binary path: ${binaryPath}`);
        if (!binaryPath) {
            log(`[milkdrop] renderer binary not found`);
            return;
        }

        const argv = this._buildRendererArgs(binaryPath, monitorIndex);
        const shellMajor = parseInt(Config.PACKAGE_VERSION.split('.')[0], 10);
        const isWayland = _isWayland();

        const launcher = new Gio.SubprocessLauncher({
            flags: Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_MERGE,
        });
        /* F3a: use Cairo to avoid GSK's GL context racing with SDL2's offscreen
         * GL context during projectM shader compilation, which causes
         * repeatable SIGSEGVs in Mesa Gallium. */
        launcher.setenv('GSK_RENDERER', 'cairo', true);
        /* GtkGLArea pode escolher GLES; projectM precisa de OpenGL desktop (GLSL 330). */
        launcher.setenv('MILKDROP_FORCE_GL_API', '1', true);
        /* Mesa Intel GPU: force OpenGL Compatibility profile for projectM shaders */
        launcher.setenv('MESA_GL_VERSION_OVERRIDE', '3.3Compatibility', true);
        this._launchers.set(monitorIndex, launcher);

        log(`[milkdrop] isWayland=${isWayland}, shellMajor=${shellMajor}`);
        let subprocess = null;
        let waylandClient = null;
        try {
            if (isWayland) {
                const metaContext = global.display.get_context();
                if (shellMajor < 49) {
                    log('[milkdrop] Using legacy WaylandClient.new path');
                    waylandClient = Meta.WaylandClient.new(metaContext, launcher);
                    subprocess = waylandClient.spawnv(global.display, argv);
                    this._waylandClients.set(monitorIndex, waylandClient);
                } else {
                    log('[milkdrop] Using new_subprocess path');
                    waylandClient = Meta.WaylandClient.new_subprocess(metaContext, launcher, argv);
                    if (!waylandClient) {
                        log('[milkdrop] Meta.WaylandClient.new_subprocess returned null');
                        subprocess = null;
                    } else {
                        this._waylandClients.set(monitorIndex, waylandClient);
                        subprocess = waylandClient.get_subprocess();
                        log('[milkdrop] WaylandClient created, subprocess obtained');
                    }
                }
            } else {
                log('[milkdrop] Using X11 path');
                subprocess = launcher.spawnv(argv);
            }
            log(`[milkdrop] subprocess created: ${subprocess ? 'yes' : 'no'}`);
        } catch (error) {
            const msg = error?.message ?? String(error);
            log(`[milkdrop] failed to spawn renderer: ${msg}`);
            subprocess = null;
            waylandClient = null;
        }

        this._launchers.delete(monitorIndex);

        if (!subprocess) {
            this._scheduleRetry(monitorIndex);
            return;
        }

        this._retrySteps.set(monitorIndex, 0);
        const stdoutCancellable = new Gio.Cancellable();
        const stdoutStream = Gio.DataInputStream.new(subprocess.get_stdout_pipe());
        this._stdoutCancellables.set(monitorIndex, stdoutCancellable);
        this._stdoutStreams.set(monitorIndex, stdoutStream);
        this._subprocesses.set(monitorIndex, subprocess);

        /* Pause / rotation / shuffle: argv covers preset-rotation-interval and --shuffle,
         * but pause state depends on fullscreen/mpris and must be pushed live. FPS is
         * passed as --fps (same default 60 as the Meson smoke test, which does not use
         * the control socket). */
        const shouldPause = this._pauseReasons.fullscreen ||
                           this._pauseReasons.maximized ||
                           this._pauseReasons.mpris;
        const rotation = this._settings.get_int('preset-rotation-interval');
        const shuffleOn = this._settings.get_boolean('shuffle');
        const initialCommands = [
            `pause ${shouldPause ? 'on' : 'off'}`,
            `rotation-interval ${rotation}`,
            `shuffle ${shuffleOn ? 'on' : 'off'}`,
        ];
        const lastPreset = this._settings.get_string('last-preset');
        if (lastPreset) {
            const wasPaused = this._settings.get_boolean('was-paused');
            initialCommands.push(`restore-state ${GLib.shell_quote(lastPreset)} ${wasPaused ? '1' : '0'}`);
        }
        sendMilkdropControlCommandsSequentially(
            initialCommands,
            getMilkdropSocketPath(monitorIndex),
            120,
            CONTROL_SOCKET_MAX_RETRIES,
            CONTROL_SOCKET_RETRY_DELAY_MS
        );

        // Setup PausePolicy for this monitor if needed
        this._setupPausePolicyForMonitor(monitorIndex);

        this._readRendererOutput(monitorIndex);

        const spawnedSubprocess = subprocess;
        const spawnedCancellable = stdoutCancellable;
        spawnedSubprocess.wait_async(spawnedCancellable, (proc, res) => {
            let exitCode = 0;
            let exitedNormally = false;
            let termSignal = 0;
            try {
                proc.wait_finish(res);
                /* GLib 2.70+: get_if_exited() avoids the g_return_if_fail
                 * in get_exit_status() when the process died abnormally. */
                if (typeof proc.get_if_exited === 'function') {
                    exitedNormally = proc.get_if_exited();
                    if (exitedNormally)
                        exitCode = proc.get_exit_status();
                    else if (typeof proc.get_term_sig === 'function')
                        termSignal = proc.get_term_sig();
                } else {
                    /* Fallback for older GLib: use get_successful() as a proxy
                     * for normal exit. If false, assume non-zero exit. */
                    exitedNormally = proc.get_successful() || false;
                    if (exitedNormally)
                        exitCode = proc.get_exit_status();
                }
            } catch (error) {
                if (!error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                    log(`[milkdrop] renderer wait failed: ${error}`);
            }

            /* Guard: only clear state if this callback belongs to the current
             * subprocess. A stale wait_async from a previously killed renderer
             * must not wipe out a newly spawned replacement (enable/disable cycle). */
            if (this._subprocesses.get(monitorIndex) !== spawnedSubprocess)
                return;

            this._stdoutStreams.delete(monitorIndex);
            this._stdoutCancellables.delete(monitorIndex);
            this._subprocesses.delete(monitorIndex);
            this._waylandClients.delete(monitorIndex);
            this._clearAnchor(monitorIndex);

            // Disable PausePolicy for this monitor
            const policy = this._pausePolicies.get(monitorIndex);
            if (policy) {
                policy.disable();
                this._pausePolicies.delete(monitorIndex);
            }

            const now = Date.now();
            const rapidDeath = (now - this._lastDeathTimestamp) < 10000;
            this._lastDeathTimestamp = now;

            if (!exitedNormally || exitCode !== 0) {
                if (rapidDeath) {
                    this._consecutiveAbnormalDeaths++;
                    log(`[milkdrop] renderer abnormal death #${this._consecutiveAbnormalDeaths} (signal=${termSignal}, code=${exitCode})`);
                } else {
                    this._consecutiveAbnormalDeaths = 1;
                    log(`[milkdrop] renderer died after stable run (signal=${termSignal}, code=${exitCode})`);
                }
            } else {
                this._consecutiveAbnormalDeaths = 0;
            }

            if (this._consecutiveAbnormalDeaths >= 5) {
                log('[milkdrop] CIRCUIT BREAKER: renderer died 5 times rapidly. Stopping respawns. Disable and re-enable the extension to retry.');
                this._circuitBreakerOpen = true;
                return;
            }

            if (this._settings?.get_boolean('enabled'))
                this._scheduleRetry(monitorIndex);
        });
    }

    /**
     * Setup PausePolicy for a specific monitor if pause settings are enabled.
     * @param {number} monitorIndex
     */
    _setupPausePolicyForMonitor(monitorIndex) {
        const pauseOnFullscreen = this._settings.get_boolean('pause-on-fullscreen');
        const pauseOnMaximized = this._settings.get_boolean('pause-on-maximized');

        if (!pauseOnFullscreen && !pauseOnMaximized)
            return;

        // Don't create duplicate policies
        if (this._pausePolicies.has(monitorIndex))
            return;

        const policy = new PausePolicy(monitorIndex, this._settings, (reason, active) => {
            this._setPauseReason(reason, active);
        });
        policy.enable();
        this._pausePolicies.set(monitorIndex, policy);
        log(`[milkdrop] PausePolicy enabled for monitor ${monitorIndex}`);
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

    _buildRendererArgs(binaryPath, monitorIndex = 0) {
        const opacity = this._settings?.get_double('opacity') ?? 1.0;
        const configured = this._settings?.get_string('preset-dir') ?? '';
        const presetDir = this._resolvePresetDir(configured);
        const shuffle = this._settings?.get_boolean('shuffle') ?? false;
        const overlay = this._settings?.get_boolean('overlay') ?? false;
        const rotationInterval = this._settings?.get_int('preset-rotation-interval') ?? 30;
        const fps = this._settings?.get_int('fps') ?? 60;
        const beatSensitivity = this._settings?.get_double('beat-sensitivity') ?? 1.0;
        const hardCutEnabled = this._settings?.get_boolean('hard-cut-enabled') ?? false;
        const hardCutSensitivity = this._settings?.get_double('hard-cut-sensitivity') ?? 2.0;
        const hardCutDuration = this._settings?.get_double('hard-cut-duration') ?? 20.0;
        const softCutDuration = this._settings?.get_double('soft-cut-duration') ?? 3.0;

        const args = [
            binaryPath,
            '--monitor', `${monitorIndex}`,
            '--opacity', `${opacity}`,
            '--fps', `${fps}`,
        ];

        if (presetDir.length > 0)
            args.push('--preset-dir', presetDir);

        if (shuffle)
            args.push('--shuffle');

        if (overlay)
            args.push('--overlay');

        args.push('--preset-rotation-interval', `${rotationInterval}`);
        args.push('--beat-sensitivity', `${beatSensitivity}`);

        if (hardCutEnabled)
            args.push('--hard-cut-enabled');

        args.push('--hard-cut-sensitivity', `${hardCutSensitivity}`);
        args.push('--hard-cut-duration', `${hardCutDuration}`);
        args.push('--soft-cut-duration', `${softCutDuration}`);

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

    _readRendererOutput(monitorIndex) {
        const stdoutStream = this._stdoutStreams.get(monitorIndex);
        const stdoutCancellable = this._stdoutCancellables.get(monitorIndex);
        if (!stdoutStream || !stdoutCancellable)
            return;

        stdoutStream.read_line_async(
            GLib.PRIORITY_DEFAULT,
            stdoutCancellable,
            (stream, res) => {
                try {
                    const [line, length] = stream.read_line_finish_utf8(res);
                    if (length > 0)
                        log(`[milkdrop][monitor ${monitorIndex}] ${line}`);
                } catch (error) {
                    if (!error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        log(`[milkdrop][monitor ${monitorIndex}] output read failed: ${error}`);
                    return;
                }

                this._readRendererOutput(monitorIndex);
            }
        );
    }

    _saveStateSync(monitorIndex) {
        const socketPath = getMilkdropSocketPath(monitorIndex);
        try {
            const socket = new Gio.Socket({
                family: Gio.SocketFamily.UNIX,
                type: Gio.SocketType.STREAM,
                protocol: Gio.SocketProtocol.DEFAULT,
            });
            socket.connect(Gio.UnixSocketAddress.new(socketPath), null);
            socket.send(new TextEncoder().encode('save-state\n'), null);
            const buf = new Uint8Array(4096);
            const nread = socket.receive(buf, null);
            socket.close();
            if (nread <= 0)
                return;
            let state;
            try {
                state = JSON.parse(new TextDecoder().decode(buf.subarray(0, nread)).trim());
            } catch (_e) {
                return;
            }
            if (typeof state.preset === 'string')
                this._settings.set_string('last-preset', state.preset);
            if (typeof state.paused === 'number')
                this._settings.set_boolean('was-paused', state.paused !== 0);
        } catch (error) {
            log(`[milkdrop] _saveStateSync failed for monitor ${monitorIndex}: ${error.message}`);
        }
    }

    _stopProcess(monitorIndex) {
        this._removeRetrySource(monitorIndex);

        const stdoutCancellable = this._stdoutCancellables.get(monitorIndex);
        if (stdoutCancellable)
            stdoutCancellable.cancel();

        const subprocess = this._subprocesses.get(monitorIndex);
        if (subprocess) {
            if (this._settings)
                this._saveStateSync(monitorIndex);
            try {
                subprocess.force_exit();
            } catch (error) {
                log(`[milkdrop] failed to stop renderer for monitor ${monitorIndex}: ${error}`);
            }
        }

        this._subprocesses.delete(monitorIndex);
        this._waylandClients.delete(monitorIndex);
        this._stdoutStreams.delete(monitorIndex);
        this._stdoutCancellables.delete(monitorIndex);

        // Disable PausePolicy for this monitor
        const policy = this._pausePolicies.get(monitorIndex);
        if (policy) {
            policy.disable();
            this._pausePolicies.delete(monitorIndex);
        }

        // Clean up ManagedWindow for this monitor to prevent signal leaks
        this._clearAnchor(monitorIndex);
    }

    _restartProcess(monitorIndex) {
        this._stopProcess(monitorIndex);
        if (this._settings?.get_boolean('enabled'))
            this._spawnProcess(monitorIndex);
    }

    _scheduleRetry(monitorIndex) {
        this._removeRetrySource(monitorIndex);
        const step = this._retrySteps.get(monitorIndex) ?? 0;
        const index = Math.min(step, RETRY_DELAYS_MS.length - 1);
        const delayMs = RETRY_DELAYS_MS[index];
        this._retrySteps.set(monitorIndex, step + 1);

        const sourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delayMs, () => {
            this._retrySourceIds.delete(monitorIndex);
            if (this._settings?.get_boolean('enabled'))
                this._spawnProcess(monitorIndex);
            return GLib.SOURCE_REMOVE;
        });
        this._retrySourceIds.set(monitorIndex, sourceId);
    }

    _removeRetrySource(monitorIndex) {
        const sourceId = this._retrySourceIds.get(monitorIndex);
        if (sourceId) {
            this._safeRemoveSource(sourceId);
            this._retrySourceIds.delete(monitorIndex);
        }
    }

    _removeAllRetrySources() {
        for (const sourceId of this._retrySourceIds.values())
            this._safeRemoveSource(sourceId);
        this._retrySourceIds.clear();
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
            return metaWindow.title?.startsWith('@milkdrop!') ?? false;
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
                    wallpaper._milkdropWallpaper = true;  // Mark wallpaper widget for detection
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

                        // Use _safeActorOperation to handle race where actor is finalized
                        // between validation and clone creation
                        const result = thisRef._safeActorOperation(rendererActor, (actor) => {
                            // Use Clutter.Clone to display the renderer window (Hanabi pattern).
                            // Clone works with Wayland surfaces and handles GL content correctly.
                            const rendererClone = new Clutter.Clone({
                                source: actor,
                                x_expand: true,
                                y_expand: true,
                            });
                            rendererClone.connect('destroy', () => {
                                log(`[milkdrop] renderer clone destroyed for monitor ${monitorIndex}`);
                            });
                            wallpaper.add_child(rendererClone);
                            log(`[milkdrop] renderer actor cloned to wallpaper for monitor ${monitorIndex}`);
                            return true;
                        }, 'clone renderer actor');

                        return result ? GLib.SOURCE_REMOVE : GLib.SOURCE_REMOVE;
                    };

                    if (attachClone() === GLib.SOURCE_CONTINUE) {
                        sourceId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {
                            // Guard: skip if extension is being disabled
                            if (thisRef._isDisabling)
                                return GLib.SOURCE_REMOVE;
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
                            thisRef._safeRemoveSource(sourceId);
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

    /**
     * Safely remove a GLib source, swallowing errors if the source
     * was already removed or is invalid.
     */
    _safeRemoveSource(sourceId) {
        if (!sourceId)
            return;
        try {
            GLib.source_remove(sourceId);
        } catch (_e) { /* ignore */ }
    }

    /**
     * Schedule a debounced background reload to prevent clone thrashing.
     * Multiple rapid calls (e.g., from monitors-changed) are coalesced into
     * a single reload after RELOAD_BACKGROUNDS_DEBOUNCE_MS quiet period.
     */
    _scheduleReloadBackgrounds() {
        if (this._reloadBackgroundsSourceId > 0) {
            this._safeRemoveSource(this._reloadBackgroundsSourceId);
            this._reloadBackgroundsSourceId = 0;
        }
        this._reloadBackgroundsSourceId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT,
            RELOAD_BACKGROUNDS_DEBOUNCE_MS,
            () => {
                this._reloadBackgroundsSourceId = 0;
                this._reloadBackgrounds();
                return GLib.SOURCE_REMOVE;
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
        // Clear all wallpaper source IDs first
        for (const sourceId of this._wallpaperSourceIds)
            this._safeRemoveSource(sourceId);
        this._wallpaperSourceIds.clear();

        // Destroy wallpaper widgets and their clones (Clutter.Clone will be cleaned up
        // automatically by the wallpaper destroy).
        for (const wallpaper of this._wallpaperActors) {
            this._safeActorOperation(wallpaper, (actor) => {
                actor.destroy();
            }, 'destroy wallpaper');
        }
        this._wallpaperActors.clear();

        this._injectionManager?.clear();
        this._injectionManager = null;

        // Reload backgrounds to remove our actors from all background contexts.
        this._reloadBackgrounds();
    }

    _clearAnchorRetrySources() {
        for (const id of this._anchorRetrySourceIds)
            this._safeRemoveSource(id);
        this._anchorRetrySourceIds.clear();
    }

    /**
     * On Wayland, owns_window() can be false on the first 'map' even for our
     * subprocess. If we return early, we never move_resize_frame() to the
     * monitor — the renderer stays at GTK default size/position (often invisible
     * or a wrong monitor). Retry a few times, then fall back to wm_class/title.
     */
    _scheduleAnchorRetries(window, attempt, monitorIndexHint = null) {
        const monitorIndex = monitorIndexHint ?? window.get_monitor();
        if (attempt === 0) {
            if (this._anchorPending.get(monitorIndex))
                return;
            this._anchorPending.set(monitorIndex, true);
        }

        const maxAttempts = 15;
        if (attempt >= maxAttempts) {
            log(`[milkdrop] owns_window still false after retries for monitor ${monitorIndex}; anchoring if title/wm_class match`);
            if (window.get_wm_class() === 'milkdrop' || window.title === 'milkdrop') {
                if (!window.get_compositor_private()) {
                    log(`[milkdrop] max-retries anchor: compositor_private null for monitor ${monitorIndex}, giving up`);
                    return;
                }
                this._anchorWindow(window);
            }
            return;
        }

        const delayMs = 50 + attempt * 40;
        let sid = 0;
        sid = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delayMs, () => {
            this._anchorRetrySourceIds.delete(sid);

            // Guard: skip if extension is being disabled
            if (this._isDisabling)
                return GLib.SOURCE_REMOVE;

            if (!this._settings?.get_boolean('enabled'))
                return GLib.SOURCE_REMOVE;

            // Check if we have a wayland client for this specific monitor
            const waylandClient = this._waylandClients.get(monitorIndex);
            if (!_isWayland() || !waylandClient) {
                // If not on Wayland or no wayland client, use title/wm_class
                if (window.get_wm_class() === 'milkdrop' || window.title === 'milkdrop') {
                    if (!window.get_compositor_private()) {
                        log(`[milkdrop] X11 anchor: compositor_private null for monitor ${monitorIndex}; retrying`);
                        this._scheduleAnchorRetries(window, attempt + 1, monitorIndex);
                        return GLib.SOURCE_REMOVE;
                    }
                    this._anchorWindow(window);
                }
                return GLib.SOURCE_REMOVE;
            }

            try {
                if (waylandClient.owns_window(window)) {
                    if (!window.get_compositor_private()) {
                        log(`[milkdrop] owns_window true but compositor_private null for monitor ${monitorIndex}; retrying`);
                        this._scheduleAnchorRetries(window, attempt + 1, monitorIndex);
                        return GLib.SOURCE_REMOVE;
                    }
                    this._anchorWindow(window);
                    return GLib.SOURCE_REMOVE;
                }
            } catch (e) {
                log(`[milkdrop] owns_window retry error for monitor ${monitorIndex}, anchoring by title/wm_class: ${e}`);
                if (window.get_wm_class() === 'milkdrop' || window.title === 'milkdrop') {
                    if (!window.get_compositor_private()) {
                        log(`[milkdrop] fallback anchor: compositor_private null for monitor ${monitorIndex}; retrying`);
                        this._scheduleAnchorRetries(window, attempt + 1, monitorIndex);
                        return GLib.SOURCE_REMOVE;
                    }
                    this._anchorWindow(window);
                }
                return GLib.SOURCE_REMOVE;
            }

            this._scheduleAnchorRetries(window, attempt + 1, monitorIndex);
            return GLib.SOURCE_REMOVE;
        });
        this._anchorRetrySourceIds.add(sid);
    }

    /**
     * Handle window-created: set DESKTOP type before Mutter sends configure events.
     * This fires when the Meta.Window is created — before map, before configure.
     */
    _onWindowCreated(window) {
        if (!window || this._isDisabling)
            return;

        // Check all our Wayland clients to find the owner
        for (const waylandClient of this._waylandClients.values()) {
            try {
                if (waylandClient.owns_window(window)) {
                    window.hide_from_window_list();
                    window.set_type(Meta.WindowType.DESKTOP);
                    log('[milkdrop] Early DESKTOP type set on window-created');
                    return;
                }
            } catch (_e) {
                // owns_window may fail transiently; fall through
            }
        }
    }

    _onWindowMapped(window) {
        if (!window)
            return;

        // Guard: skip if extension is being disabled
        if (this._isDisabling) {
            log('[milkdrop] _onWindowMapped: skipping, extension is disabling');
            return;
        }

        const wmClass = window.get_wm_class();
        const title = window.title;
        const isWayland = _isWayland();

        // Get the monitor index for this window
        const monitorIndex = window.get_monitor();

        // On Wayland, check all our wayland clients to find which one owns this window
        if (isWayland) {
            let foundOwner = false;
            for (const [idx, waylandClient] of this._waylandClients.entries()) {
                try {
                    if (waylandClient.owns_window(window)) {
                        foundOwner = true;
                        log(`[milkdrop] Window matched by wayland client for monitor ${idx}`);
                        this._anchorWindow(window);
                        return;
                    }
                } catch (error) {
                    // Ownership API failed, continue to next client
                    continue;
                }
            }

            // If no wayland client owns it, but title matches, schedule retries
            if (!foundOwner && (wmClass === 'milkdrop' || title === 'milkdrop')) {
                log('[milkdrop] owns_window false at map; scheduling anchor retries');
                this._scheduleAnchorRetries(window, 0, monitorIndex);
                return;
            }

            // Not our window
            return;
        }

        // X11: match by wm_class or title
        if (wmClass !== 'milkdrop' && title !== 'milkdrop')
            return;

        log(`[milkdrop] renderer window matched — anchoring on monitor ${monitorIndex}`);
        this._anchorWindow(window);
    }

    _anchorWindow(window) {
        let monitorIndex = window.get_monitor();
        if (monitorIndex < 0)
            monitorIndex = 0;
        this._anchorPending.set(monitorIndex, false);

        // Shell 49+: canonical background window type setup
        // Fallback — normally _onWindowCreated() sets this before map.
        // Kept for X11 and for the owns_window-retry code path.
        const shellMajor = parseInt(Config.PACKAGE_VERSION.split('.')[0], 10);
        if (shellMajor >= 49) {
            try {
                window.hide_from_window_list();
                window.set_type(Meta.WindowType.DESKTOP);
            } catch (e) {
                log(`[milkdrop] Shell 49+ window setup failed: ${e}`);
            }
        }

        // Validate window before proceeding
        if (!this._isWindowValid(window)) {
            log(`[milkdrop] ERROR: Invalid window for anchoring on monitor ${monitorIndex}`);
            return;
        }

        const actor = window.get_compositor_private();
        log(`[milkdrop] _anchorWindow monitor ${monitorIndex}: actor=${actor}, visible=${actor?.visible}`);

        // Validate actor before proceeding
        if (!this._isActorValid(actor)) {
            log(`[milkdrop] ERROR: No compositor actor for window or actor is disposed on monitor ${monitorIndex}`);
            return;
        }

        // Disable any previous ManagedWindow before re-anchoring
        const existingManaged = this._managedWindows.get(monitorIndex);
        if (existingManaged) {
            existingManaged.disable();
            this._managedWindows.delete(monitorIndex);
            log(`[milkdrop] Disabled previous ManagedWindow for monitor ${monitorIndex}`);
        }

        // Create ManagedWindow with callbacks for events
        const managed = new ManagedWindow(window, monitorIndex, {
            onRaised: (w) => {
                log(`[milkdrop] ManagedWindow raised callback for monitor ${monitorIndex}`);
            },
            onMoved: (w) => {
                // Re-enforce coverage if window position changed unexpectedly
                this._enforceWindowCoverage(w);
            },
        });

        // Get monitor geometry for anchoring (fallback to monitor 0 for virtual monitors)
        let geometry = global.display.get_monitor_geometry(monitorIndex);
        if (!geometry || geometry.width <= 0 || geometry.height <= 0) {
            const fallbackMonitor = Main.layoutManager.monitors[0];
            if (fallbackMonitor) {
                geometry = fallbackMonitor.geometry;
                log(`[milkdrop] Falling back to monitor 0 geometry: ${geometry.width}x${geometry.height}`);
            }
        }
        if (!geometry || geometry.width <= 0 || geometry.height <= 0) {
            log(`[milkdrop] ERROR: Invalid monitor geometry for anchoring on monitor ${monitorIndex}`);
            managed.disable();
            return;
        }

        // Use ManagedWindow.anchor() which handles:
        // - Detecting wallpaper parent vs window_group
        // - Setting reparentState
        // - Moving actor to window_group (if needed)
        // - Sticking to all workspaces
        // - Enforcing coverage
        // - Lowering window
        managed.anchor(geometry);

        // Track reparent state in extension for compatibility
        this._actorReparentState.set(actor, managed.state.reparentState);

        // Store the managed window
        this._managedWindows.set(monitorIndex, managed);
        this._consecutiveAbnormalDeaths = 0;
        log(`[milkdrop] renderer window anchored successfully on monitor ${monitorIndex} (using ManagedWindow)`);

        // Trigger wallpaper injection after window is anchored
        this._scheduleReloadBackgrounds();
    }

    _enforceWindowCoverage(window) {
        let monitorIndex = window.get_monitor();
        if (monitorIndex < 0) {
            // Find which monitor this window belongs to by checking managed windows
            for (const [idx, managed] of this._managedWindows.entries()) {
                if (managed.window === window) {
                    monitorIndex = idx;
                    break;
                }
            }
        }
        if (monitorIndex < 0)
            monitorIndex = this._settings?.get_int('monitor') ?? 0;
        log(`[milkdrop] _enforceWindowCoverage: monitor=${monitorIndex}`);

        const geometry = global.display.get_monitor_geometry(monitorIndex);
        log(`[milkdrop] Monitor ${monitorIndex} geometry: ${geometry?.x},${geometry?.y} ${geometry?.width}x${geometry?.height}`);
        if (!geometry || geometry.width <= 0 || geometry.height <= 0) {
            log(`[milkdrop] ERROR: Invalid monitor geometry for monitor ${monitorIndex}`);
            return;
        }

        // If we have a ManagedWindow, use its enforceCoverage method
        const managed = this._managedWindows.get(monitorIndex);
        if (managed && managed.window === window) {
            managed.enforceCoverage(geometry);
            return;
        }

        // Fallback for windows not yet in _managedWindows (shouldn't happen normally)
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

    _clearAnchor(monitorIndex) {
        this._anchorPending.set(monitorIndex, false);

        const managed = this._managedWindows.get(monitorIndex);
        if (managed) {
            // Remove actor from reparent state tracking
            const actor = managed.window?.get_compositor_private();
            if (actor)
                this._actorReparentState.delete(actor);

            // Disable the ManagedWindow (disconnects signals, clears references)
            managed.disable();
            this._managedWindows.delete(monitorIndex);
            log(`[milkdrop] Cleared ManagedWindow for monitor ${monitorIndex}`);
        }
    }

    _clearAllAnchors() {
        this._clearAnchorRetrySources();

        for (const monitorIndex of this._managedWindows.keys())
            this._clearAnchor(monitorIndex);

        this._actorReparentState.clear();
        this._anchorPending.clear();
    }

    _disconnectWmSignals() {
        for (const id of this._wmSignalIds)
            global.window_manager.disconnect(id);
        this._wmSignalIds = [];

        if (this._windowCreatedId) {
            global.display.disconnect(this._windowCreatedId);
            this._windowCreatedId = 0;
        }
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

    _isActorValid(actor) {
        if (!actor)
            return false;
        // Check if actor is disposed/finalized - try multiple methods for compatibility
        if (typeof actor.is_finalized === 'function' && actor.is_finalized())
            return false;
        if (typeof actor.is_destroyed === 'function' && actor.is_destroyed())
            return false;
        // Check if actor has a stage - detached actors are invalid
        if (typeof actor.get_stage === 'function' && !actor.get_stage())
            return false;
        return typeof actor.get_parent === 'function';
    }

    _isWindowValid(window) {
        return window &&
               typeof window.get_compositor_private === 'function' &&
               window.get_compositor_private() !== null;
    }

    /**
     * Safely execute an operation on an actor with race condition protection.
     * Actors can be finalized between validation check and use, so this helper
     * wraps operations in try/catch and re-validates before use.
     * @param {Clutter.Actor} actor - The actor to operate on
     * @param {function(Clutter.Actor):any} operation - The operation to perform
     * @param {string} operationName - Name of operation for logging
     * @returns {any} Result of operation, or undefined if actor was invalid
     */
    _safeActorOperation(actor, operation, operationName = 'operation') {
        if (!this._isActorValid(actor)) {
            log(`[milkdrop] _safeActorOperation: actor invalid before ${operationName}`);
            return undefined;
        }

        try {
            return operation(actor);
        } catch (e) {
            // Check if error is due to actor finalization
            const errorStr = String(e);
            if (errorStr.includes('finalized') ||
                errorStr.includes('destroyed') ||
                errorStr.includes('disposed')) {
                log(`[milkdrop] _safeActorOperation: actor finalized during ${operationName}`);
            } else {
                log(`[milkdrop] _safeActorOperation: ${operationName} failed: ${e}`);
            }
            return undefined;
        }
    }
}