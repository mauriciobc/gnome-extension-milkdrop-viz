# GTK4, GtkGLArea, and libprojectM Integration

> **Architecture Note (2025-04)**: The production renderer uses an **SDL2 offscreen context** (custom FBO + `glReadPixels` + `GtkPicture`) rather than `GtkGLArea`. The canonical `GtkGLArea` patterns documented below remain valid for **test and reference code** (`tests/test_gtk_glarea_projectm.c`, `tests/reference_renderer.c`). The production frame flow is described in [14-renderer-sdl2-offscreen-architecture.md](14-renderer-sdl2-offscreen-architecture.md).

## Core rule

All OpenGL-dependent `libprojectM` calls must happen only when the renderer's GL context is current. For the production path this means inside the SDL2 offscreen frame sequence; for test/reference code this means inside the `GtkGLArea` render lifecycle.

This is the single most important runtime constraint in the renderer.

## Why this matters

The PRD's embedding strategy depends on `projectm_render_frame()` drawing into the currently bound framebuffer, while the renderer process owns the current context and render target. Any attempt to issue projectM rendering calls from control-thread code or before the GL environment is ready risks undefined behavior or crashes.

## Initialization sequence (test / reference path)

A safe initialization sequence when using `GtkGLArea` is:

1. Create GTK application and window.
2. Create `GtkGLArea` and attach render and resize handlers.
3. Realize the GLArea and ensure context creation succeeds.
4. Create the projectM handle.
5. Set initial window size and FPS.
6. Load an initial preset only after the GL environment is ready.

If projectM creation can occur before preset loading, that is preferred, but any GL-resource-dependent calls should be treated as requiring a current context.

## Production frame flow (SDL2 offscreen)

The production renderer uses `OffscreenRenderer` (`src/offscreen_renderer.c`) to manage an SDL2 hidden window and a custom FBO:

1. Drain available stereo floats from the ring buffer.
2. Feed samples to `projectm_pcm_add_float(...)`.
3. Consume one pending preset change if present.
4. Call `offscreen_renderer_begin_frame()` to bind the SDL2 FBO and get its ID.
5. Set projectM frame time via `projectm_set_frame_time()`.
6. Call `projectm_opengl_render_frame_fbo(...)` with the current FBO ID.
7. **CRITICAL**: Call `glFinish()` to synchronize multi-pass blur rendering (see below).
8. Restore GL state in `offscreen_renderer_finish_gpu()` (see GL state policy section).
9. Call `offscreen_renderer_read_rgba()` to read pixels into a `GdkMemoryTexture` displayed by `GtkPicture`.

### Multi-Pass Blur Synchronization

**Added 2025-04-11**: ProjectM's blur effects (Gaussian blur, etc.) require multiple rendering passes:

- **Pass 0**: Horizontal blur → intermediate texture
- **Pass 1**: Vertical blur → final blur texture
- Repeated for each blur level (blur1, blur2, blur3)

**Problem**: `projectm_opengl_render_frame_fbo()` queues all GL commands but does NOT synchronize. Without explicit synchronization, subsequent framebuffer reads (like `milkdrop_probe_startup_pixels()`) may capture intermediate rendering state instead of the final output.

**Solution**: Call `glFinish()` immediately after `projectm_opengl_render_frame_fbo()`:

```c
projectm_opengl_render_frame_fbo(app_data->projectm, target_fbo);
offscreen_renderer_finish_gpu(app_data->offscreen);
```

Inside `offscreen_renderer_finish_gpu()`:
```c
/* Block CPU until all projectM multi-pass blur commands complete on the GPU.
 * Without this, pixel readback may capture an intermediate blur pass
 * instead of the final frame. */
glFinish();
```

**Reference**: The official SDL example (`reference_codebases/projectm/src/sdl-test-ui/pmSDL.cpp:453-455`) relies on `SDL_GL_SwapWindow()` for implicit synchronization. Our readback path has no implicit swap, so explicit sync is required.

**Performance**: `glFinish()` overhead is minimal (<1ms) as rendering is GPU-bound. This is a correctness requirement, not an optimization.

**Location**: `src/offscreen_renderer.c:239-260` (`offscreen_renderer_finish_gpu()`)

## GL state policy

**Updated 2025-04-11**: After analyzing projectM's internal rendering implementation and SDL2 context requirements, comprehensive GL state restoration is required after `projectm_opengl_render_frame_fbo()`.

### Required State Restoration

The following GL state must be restored after projectM rendering:

```c
glUseProgram(0);                  // Unbind shader programs
glBindTexture(GL_TEXTURE_2D, 0);  // Unbind textures
glActiveTexture(GL_TEXTURE0);     // Reset to texture unit 0
glDisable(GL_BLEND);              // Disable blending
glDisable(GL_DEPTH_TEST);         // Disable depth test
glDisable(GL_STENCIL_TEST);       // Disable stencil test
glDisable(GL_SCISSOR_TEST);       // Disable scissor test
```

**Rationale**: While projectM cleans up most GL state internally (see `reference_codebases/projectm/src/libprojectM/Renderer/CopyTexture.cpp:272-275` and `BlurTexture.cpp:267`), leaving the context in a known clean state prevents rendering artifacts and simplifies debugging.

**Location**: `src/offscreen_renderer.c:248-254` (`offscreen_renderer_finish_gpu()`)

This policy is a correctness requirement, not a performance optimization.

## Resize handling

On resize, the renderer must update projectM with the correct pixel size so its internal framebuffers are recreated appropriately. If only logical size is passed on HiDPI systems, the visualization may appear blurred.

Therefore, the resize path should compute real pixel dimensions based on the widget scale factor or framebuffer dimensions before calling the projectM resize API.

## HiDPI handling

HiDPI is not optional. The PRD notes that projectM expects pixel dimensions, while GTK often presents logical dimensions to application code. The docs for this project must therefore require scale-factor-aware resizing and initial sizing.

## GLArea error handling (test / reference path)

If context creation fails or GTK reports a GL error state, the renderer should not crash with a vague failure. It should surface a clear status, disable rendering, and remain debuggable.

## Window opacity

Opacity changes should be handled on the GTK main thread. The socket command should update a render-owned or main-thread-safe value, and the GTK thread should apply it using widget/window APIs.

## ProjectM ownership rules

- The projectM handle is render-owned state.
- Preset loading is logically render-owned because it may trigger shader compilation and GL resource work.
- The control thread may request changes only through a handoff flag or queued command.

## Realization and teardown

Destruction order matters. On shutdown:

1. Stop future control commands.
2. Stop or detach audio capture callbacks safely.
3. Ensure no future render pulse will use freed projectM state.
4. Destroy the projectM handle while the GL lifetime assumptions remain valid.
5. Destroy the GTK window and exit the main loop.

## Agent rules

Any code change that introduces GL calls outside the render path must be treated as a design violation unless accompanied by a clear context-current proof and updated documentation.
