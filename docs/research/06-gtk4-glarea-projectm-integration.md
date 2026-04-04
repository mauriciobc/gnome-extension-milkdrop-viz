# GTK4, GtkGLArea, and libprojectM Integration

## Core rule

`GtkGLArea` owns the OpenGL context used by the renderer window. Therefore, all OpenGL-dependent `libprojectM` calls must happen only when the GTK-managed context is current, which for this project means inside the GLArea render lifecycle.

This is the single most important runtime constraint in the renderer.

## Why this matters

The PRD’s embedding strategy depends on `projectm_render_frame()` drawing into the currently bound framebuffer, while GTK manages the current context and render target. Any attempt to issue projectM rendering calls from control-thread code or before the GLArea is realized risks undefined behavior or crashes.

## Initialization sequence

A safe initialization sequence is:

1. Create GTK application and window.
2. Create `GtkGLArea` and attach render and resize handlers.
3. Realize the GLArea and ensure context creation succeeds.
4. Create the projectM handle.
5. Set initial window size and FPS.
6. Load an initial preset only after the GL environment is ready.

If projectM creation can occur before preset loading, that is preferred, but any GL-resource-dependent calls should be treated as requiring a current context.

## Render callback contract

The render callback should be treated as the sole place where projectM frame rendering occurs.

Recommended per-frame order:

1. Drain available stereo floats from the ring buffer.
2. Feed samples to `projectm_pcm_add_float(...)`.
3. Consume one pending preset change if present.
4. Call `projectm_render_frame(...)`.
5. Restore any GTK-sensitive GL state that testing confirms must be restored.
6. Return success without redundant clears.

## GL state policy

The PRD assumes GTK only validates a narrow subset of GL state after render, specifically depth and stencil enablement. Until proven otherwise, restore only the state GTK checks rather than attempting an expensive “restore everything” wrapper around projectM.

This policy should remain documented as a performance decision backed by integration testing.

## Resize handling

On `GtkGLArea` resize, the renderer must update projectM with the correct pixel size so its internal framebuffers are recreated appropriately. If only logical size is passed on HiDPI systems, the visualization may appear blurred.

Therefore, the resize path should compute real pixel dimensions based on the widget scale factor or framebuffer dimensions before calling the projectM resize API.

## HiDPI handling

HiDPI is not optional. The PRD notes that projectM expects pixel dimensions, while GTK often presents logical dimensions to application code. The docs for this project must therefore require scale-factor-aware resizing and initial sizing.

## GLArea error handling

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
3. Ensure no future render callback will use freed projectM state.
4. Destroy the projectM handle while the GL lifetime assumptions remain valid.
5. Destroy the GTK window and exit the main loop.

## Agent rules

Any code change that introduces GL calls outside the render path must be treated as a design violation unless accompanied by a clear context-current proof and updated documentation.
