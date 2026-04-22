# Renderer SDL2 Offscreen Architecture

## Why SDL2 Offscreen Instead of GtkGLArea?

The production renderer uses an **SDL2 hidden window + custom FBO** instead of rendering directly into a `GtkGLArea`. This document explains the rationale so future architectural decisions preserve the invariant.

## Architecture Overview

```
┌─────────────────────────────────────────┐
│  GTK4 Application (main thread)         │
│  ┌─────────────────────────────────┐    │
│  │  GtkWindow                      │    │
│  │  └─ GtkPicture                  │    │
│  │     (displays GdkMemoryTexture) │    │
│  └─────────────────────────────────┘    │
│                    ▲                    │
│         glReadPixels upload             │
│                    │                    │
│  ┌─────────────────────────────────┐    │
│  │  SDL2 Offscreen Context         │    │
│  │  ┌─ hidden SDL_Window           │    │
│  │  └─ custom FBO (render_fbo)     │    │
│  │     └─ projectM render target   │    │
│  └─────────────────────────────────┘    │
└─────────────────────────────────────────┘
```

## Key Design Decisions

### 1. Context Isolation from GTK4's GL State

`GtkGLArea` shares the GDK GL context with GTK's internal rendering. GTK4 may change FBO bindings, viewport state, or shader programs at any time during the frame pipeline. By using a separate SDL2 context:

- **projectM owns its GL state entirely**. No risk of GTK internals interfering with FBO bindings between frames.
- **No need for `gtk_gl_area_attach_buffers()` mid-frame**. The SDL2 FBO is the only target for the lifetime of the render pulse.
- **Simpler mental model**: the renderer subprocess is fully responsible for its GL state; the GTK side is just a pixel consumer.

### 2. Stability Under GNOME Shell Compositor Integration

The renderer runs as a **separate subprocess** spawned by the GNOME Shell extension. If the renderer used `GtkGLArea`, its window would be a GTK4 GL surface managed by the compositor. Historical issues with this approach:

- **Mesa/Vulkan deadlock & GL context contention**: GTK4's default Vulkan renderer (`GSK_RENDERER=vulkan`) can enter an uninterruptible D-state when the Wayland presentation path races with the DRM kernel writer.  Forcing `GSK_RENDERER=gl` avoids the deadlock but creates a second OpenGL context (GSK's + SDL2's) that races during projectM shader compilation, causing repeatable SIGSEGVs in Gallium (`projectm_load_preset_file → libgallium`).  The production spawn therefore uses `GSK_RENDERER=cairo` (software compositing); the renderer window is just a `GtkPicture` displaying a CPU `GdkMemoryTexture`, so the overhead is negligible.
- **Compositor lifetime mismatches**: If the Shell hides or repositions the renderer window, GTK may unrealize the `GdkSurface` and destroy the GL context. With SDL2 offscreen, the GL context lifetime is independent of the GTK window's mapped state.

### 3. `GtkPicture` + `GdkMemoryTexture` Upload Path

Instead of rendering directly to the screen, the renderer:

1. Renders into the SDL2 FBO.
2. Calls `glReadPixels()` to read RGBA data into CPU memory (`offscreen_renderer_read_rgba()`).
3. Wraps the pixel buffer in a `GdkMemoryTexture`.
4. Displays it via `gtk_picture_set_paintable()`.

**Trade-offs**:
- **CPU↔GPU copy overhead**: `glReadPixels` adds a synchronous readback. This is acceptable because the renderer targets a wallpaper/background use case where latency is not critical.
- **Simplicity**: No need to manage shared GL textures or `GdkGLTexture` across process boundaries. The extension simply sees a normal GTK window with a `GtkPicture`.
- **Opacity control**: `gtk_widget_set_opacity()` works naturally on the `GtkWindow` without GL blending state complications.

### 4. Test/Reference Path Still Uses GtkGLArea

The **canonical GtkGLArea pattern** is preserved in:
- `tests/test_gtk_glarea_projectm.c` — automated test for GtkGLArea + projectM FBO integration
- `tests/reference_renderer.c` — ground-truth renderer for blur validation

These exist to:
- Validate that projectM works correctly inside GTK4's GL lifecycle when needed.
- Serve as a reference if the production renderer ever needs to migrate back to GtkGLArea.
- Prove compliance with GTK4 FBO guidelines (see `docs/research/06-gtk4-glarea-projectm-integration.md`).

## Frame Lifecycle (Production)

```c
// 1. Render pulse fires (~60 Hz via g_timeout_add)
milkdrop_render_frame(app_data);

// 2. Ensure SDL2 context is current
offscreen_renderer_make_current(app_data->offscreen);

// 3. Bind FBO, clear, return FBO ID
offscreen_renderer_begin_frame(..., &target_fbo);

// 4. Feed audio, set frame time, load pending preset
// ...

// 5. Render into SDL2 FBO
projectm_opengl_render_frame_fbo(app_data->projectm, target_fbo);

// 6. Restore GL state + GPU sync (blur)
offscreen_renderer_finish_gpu(app_data->offscreen);

// 7. Read pixels
offscreen_renderer_read_rgba(app_data->offscreen, &pixels, ...);

// 8. Upload to GtkPicture
milkdrop_publish_frame_to_picture(app_data, pixels, ...);
```

## Migration Considerations

If a future agent proposes migrating the production renderer **back** to `GtkGLArea`, they must address:

1. **Mesa/Vulkan deadlock & GL context contention**: GTK4's Vulkan renderer is explicitly disabled (`GSK_RENDERER=cairo`) because of a documented rw-semaphore deadlock, and the GL renderers are avoided because their second GL context races with SDL2's offscreen context during projectM shader compilation. Moving to `GtkGLArea` would re-expose both issues unless they are confirmed fixed in the target Mesa/GTK versions.
2. **Compositor context destruction**: GNOME Shell may unmap/realize the renderer window during overview transitions. The `GtkGLArea` context would need to survive these events or be recreated safely.
3. **FBO binding guarantees**: `gtk_gl_area_attach_buffers()` must be called before every `projectm_opengl_render_frame_fbo()`, and GL state must be restored afterward. The SDL2 path avoids this overhead.
4. **Performance**: `glReadPixels` overhead would be eliminated, but the benefit is marginal for a ~60 Hz background renderer.

## Agent Rules

- **Do not introduce `GtkGLArea` into the production renderer** without a written rationale addressing the four migration considerations above.
- **Do not remove SDL2 offscreen code** without confirming the replacement path handles context recreation, Mesa deadlock avoidance, and GNOME Shell compositor integration.
- **Preserve the test code** (`tests/test_gtk_glarea_projectm.c`, `tests/reference_renderer.c`) as the canonical GtkGLArea reference.
