# Window and display map

## Where content is shown

### 1. Renderer process (GTK app)

- **Entry:** `src/renderer/renderer.js` – Gtk application, `_onActivate()`.
- **Window:** One `Gtk.ApplicationWindow` per monitor, title `@io.github.mauriciobc.MilkdropRenderer!{...}|N`.
- **Content:** `MilkdropGLArea` (optionally inside `Gtk.GraphicsOffload`) draws:
  - **Helper ready:** Texture from native GL helper (SHM/FD frame pixels).
  - **Fallback:** `glarea.js` `vfunc_snapshot()` – when `!_helperReady || !helperTexture`, draws stripes/orbs from `_buildFallbackFrameState()`; driven by `_ensureFallbackTick()` (tick callback that calls `queue_draw()`).
- **Result:** One Wayland (or X11) window per monitor. The **fallback** is the software-rendered placeholder (stripes/orbs) inside this same window.

### 2. Shell side – window actor

- **Creation:** Mutter creates a window actor when the renderer window is mapped.
- **Lookup:** `global.get_window_actors(false)` – list of all window actors (false = include renderer; default filter hides it).
- **Matching:** `actor.meta_window.title?.startsWith(RENDERER_TITLE_PREFIX)` with `RENDERER_TITLE_PREFIX = '@io.github.mauriciobc.MilkdropRenderer!'`.
- **Management:** `monitor.js` – `ManagedRendererWindow` (position, type DESKTOP, etc.). Opacity/visibility for “only when media playing” is applied in `_applyMediaOverlayVisibility()` to every actor whose window title starts with that prefix.

### 3. Shell side – LiveWallpaper (clone)

- **Where:** `gnomeShellOverride.js` – overrides `Background.BackgroundManager._createBackgroundActor`, adds a `LiveWallpaper` on top of each **background actor**.
- **What:** `wallpaper.js` – `LiveWallpaper` is an `St.Widget` that creates a `Clutter.Clone` with `source: renderer` (the renderer **window actor** from `get_window_actors(false)`).
- **Used in:** Overview, lock screen, workspace thumbnails (background stack). The **desktop** view may or may not use the same background actor depending on shell version.
- **Visibility:** `setMediaOverlayVisibility(visible)` on `GnomeShellOverride` sets opacity on all `_wallpaperActors` (the LiveWallpaper widgets).

## Flow summary

```
Renderer process                    Shell (Mutter)
─────────────────                   ──────────────
Gtk.ApplicationWindow  ──map──>    Window actor (get_window_actors)
  └─ MilkdropGLArea                  └─ opacity set in _applyMediaOverlayVisibility
       └─ fallback (snapshot)              
            or helper texture       BackgroundManager (per monitor)
                                     └─ LiveWallpaper (clone of window actor)
                                          └─ opacity set in setMediaOverlayVisibility
```

## “Fallback” definition

- **Fallback** = the Gtk snapshot content drawn in `glarea.js` when the native helper is not ready or has no frame (stripes/orbs). It is **inside** the same Gtk window. Making that window’s actor fully transparent (opacity 0) or non-visible should hide the fallback too.

## Possible reasons fallback stays visible

1. **Window actor not found** – title mismatch, or actor not yet in `get_window_actors(false)` when we run.
2. **Opacity not applied** – e.g. actor is a container and compositor draws the surface from a child.
3. **Different view** – desktop might use another code path that doesn’t use the same actor list or ignores opacity.
4. **Visibility needed** – setting `actor.visible = false` in addition to `opacity = 0`.
