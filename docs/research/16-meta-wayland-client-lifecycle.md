# Meta.WaylandClient Lifecycle for Background Renderer Injection

## Purpose

This document covers the complete `Meta.WaylandClient` API as used to spawn the milkdrop
renderer binary and inject its Wayland surface as a background compositor actor. It
includes the API redesign that landed in GNOME Shell 49, which is a hard breakage.

## API Versions

| Shell | API style | Constructor |
|-------|-----------|-------------|
| ≤ 48  | Legacy `spawnv` | `Meta.WaylandClient` + `spawnv()` |
| 49+   | New subprocess | `Meta.WaylandClient.new_subprocess()` |

Shell 49 removed `spawnv()` entirely. Extensions must branch or drop support for ≤ 48.

## Constructor and Methods (Shell 49+)

```javascript
// GJS — Shell 49+
const launcher = new Gio.SubprocessLauncher(
    { flags: Gio.SubprocessFlags.NONE }
);
const client = Meta.WaylandClient.new_subprocess(
    Meta.get_backend().get_context(),  // MetaContext
    launcher,                          // GSubprocessLauncher
    ['/path/to/milkdrop', '--arg']     // argv (string[])
);
if (client === null) throw new Error('failed to spawn renderer');
```

**`Meta.WaylandClient.new_subprocess(context, launcher, argv)`**
- Spawns the binary, creates a private Wayland socket, and injects `WAYLAND_SOCKET` into
  the child environment so the child connects to the compositor without going through the
  public `WAYLAND_DISPLAY` socket.
- Returns `null` on error (GError details visible only in C; catch the null in GJS).
- Caller must hold a strong reference for the lifetime of the child process. Dropping the
  last reference kills the child.

**`client.owns_window(metaWindow)`** → `boolean`
- Returns `true` if the given `Meta.Window` was created by this client instance.
- Use on the `window-created` signal to identify which window belongs to the renderer.
- Essential: multiple windows may appear concurrently; this is the only reliable filter.

## Window Method Relocation in Shell 49

These methods moved from `Meta.WaylandClient` to `Meta.Window`:

| Before Shell 49 | Shell 49+ |
|----------------|-----------|
| `client.hide_from_window_list(window)` | `window.hide_from_window_list()` |
| `client.show_in_window_list(window)` | `window.show_in_window_list()` |
| `client.set_type(window, type)` | `window.set_type(type)` |

## Full Spawn-and-Inject Lifecycle

```javascript
enable() {
    // 1. Spawn
    this._launcher = new Gio.SubprocessLauncher({
        flags: Gio.SubprocessFlags.NONE,
    });
    this._client = Meta.WaylandClient.new_subprocess(
        Meta.get_backend().get_context(),
        this._launcher,
        [this._binaryPath, '--monitor', String(monitorIndex)]
    );

    // 2. Wait for window
    this._windowCreatedId = global.display.connect(
        'window-created',
        (display, window) => this._onWindowCreated(window)
    );
}

_onWindowCreated(window) {
    if (!this._client?.owns_window(window))
        return;

    global.display.disconnect(this._windowCreatedId);
    this._windowCreatedId = 0;
    this._injectWindow(window);
}

// 3. Inject as background actor
_injectWindow(window) {
    window.hide_from_window_list();            // Shell 49+
    window.set_type(Meta.WindowType.DESKTOP);  // prevents focus / stacking

    const windowActor = window.get_compositor_private();  // Meta.WindowActor
    const surfaceActor = windowActor.get_first_child();   // MetaSurfaceActorWayland

    // Insert below all windows at the wallpaper layer
    const backgroundGroup = Main.layoutManager._backgroundGroup;
    backgroundGroup.add_child(surfaceActor);
}

disable() {
    if (this._windowCreatedId) {
        global.display.disconnect(this._windowCreatedId);
        this._windowCreatedId = 0;
    }
    // Dropping _client reference kills the child process
    this._client = null;
    this._launcher = null;
}
```

## Obtaining MetaSurfaceActor

Mutter's compositor object hierarchy for a Wayland client window:

```
Meta.Window
  └── Meta.WindowActor   (window.get_compositor_private())
        └── Meta.SurfaceActor  (windowActor.get_first_child())
              └── Meta.SurfaceActorWayland  (Wayland-specific subclass)
```

`Meta.SurfaceActorWayland` holds the actual `wl_surface` content. This is the Clutter
actor to inject into the scene graph for rendering.

## hide_from_window_list and set_type

`hide_from_window_list()` marks the window so it does not appear in:
- Alt+Tab window switcher
- Taskbars and dock launchers
- Window pagers
- `wmctrl -l` / similar

`set_type(Meta.WindowType.DESKTOP)` additionally prevents the window from:
- Receiving keyboard focus via normal focus-follows-mouse or click-to-focus
- Being raised by window stacking operations
- Participating in workspace switching

Both are required for a background renderer that must be visually present but logically
absent from the desktop session.

## Wayland-Only Guard

```javascript
enable() {
    if (!Meta.is_wayland_compositor()) {
        log('[milkdrop] X11 session — background injection not supported');
        return;
    }
    // ... proceed
}
```

The entire `Meta.WaylandClient` API is Wayland-specific. On X11/XWayland sessions the
class may exist but `new_subprocess()` will return `null` or behave incorrectly.

## Known Constraints

- **Process lifetime:** The `Meta.WaylandClient` object owns the child process. There is no
  way to detach and re-adopt the process — it is tied to the client object lifetime.
- **No socket path exposure:** The Wayland socket path injected into `WAYLAND_SOCKET` is
  not accessible from GJS. The child connects automatically; the extension does not need it.
- **Error detail:** `new_subprocess()` returns `null` on failure with no GJS-visible error
  message. Enable `G_MESSAGES_DEBUG=all` in the child environment to debug startup failures.
- **One window per client:** The API tracks one Wayland surface per `WaylandClient`
  instance. For multi-monitor, spawn one `Meta.WaylandClient` per monitor.

## Canonical Sources

- Mutter API — `Meta.WaylandClient.new_subprocess`: https://mutter.gnome.org/meta/ctor.WaylandClient.new_subprocess.html
- Mutter API — `Meta.Window`: https://mutter.gnome.org/meta/class.Window.html
- GJS extensions guide — Shell 49 upgrade: https://gjs.guide/extensions/upgrading/gnome-shell-49.html
- Mutter source — `meta-wayland-client.c`: https://gitlab.gnome.org/GNOME/mutter/-/blob/main/src/wayland/meta-wayland-client.c
