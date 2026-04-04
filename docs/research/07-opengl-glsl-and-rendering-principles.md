# OpenGL, GLSL, and Rendering Principles

## Scope of this document

Version 2 deliberately removes the custom shader pipeline from the project scope, but OpenGL and GLSL knowledge still matters because the system embeds a third-party renderer into a GTK-owned OpenGL context.

The goal of this document is not to redesign projectM. It is to define the constraints that keep the embedder safe, performant, and debuggable.

## Rendering ownership

There should be exactly one effective rendering owner for the visualization frame: `libprojectM`. The application glue code should not become a second renderer by layering ad hoc GL passes, custom post-processing, or unrelated stateful drawing into the same callback without a design review.

This principle preserves the simplification the PRD was written to achieve.

## State discipline

Modern OpenGL programming depends heavily on explicit state ownership. Even when the application does not implement its own shaders, it must assume that framebuffer bindings, viewport, blend state, depth state, stencil state, and program bindings are mutable global state.

For this project:

- Assume projectM mutates OpenGL state.
- Restore only the minimal state GTK requires unless testing demonstrates additional requirements.
- Do not sprinkle unrelated GL calls before or after projectM rendering casually.

## Shader compilation reality

Preset switching can trigger shader compilation or related GPU pipeline work, which may stall the render thread. The PRD already treats this as expected. The correct engineering response in v2 is not to eliminate the stall at all costs, but to isolate it to preset-switch moments, document it, and keep the rest of the frame loop lean.

## FBO and render target expectations

The embedder must assume projectM renders to the currently bound framebuffer. Therefore:

- The target framebuffer must be valid when `projectm_render_frame()` is called.
- The render callback must not accidentally rebind away from the GTK target unless there is a documented reason.
- Any future custom rendering pass must define exact ownership of framebuffer binding transitions.

## Viewport correctness

Resizing and HiDPI changes affect both framebuffer dimensions and viewport assumptions. Future bugs in this area will often present as stretched, clipped, or blurry visuals. Testing must therefore include scale-factor changes and monitor moves.

## GLSL knowledge that still matters

Even without authoring projectM shaders, developers and agents should understand these concepts:

- Shader compile latency can affect perceived responsiveness.
- Fragment-heavy effects scale with resolution and can become expensive on HiDPI outputs.
- Uniform/state changes across presets may invalidate assumptions about cached state.
- Driver differences can surface only on certain GPUs or Mesa versions.

## Debugging approach

If rendering fails:

1. Confirm a valid GL context exists.
2. Confirm the render callback is firing.
3. Confirm framebuffer size is correct.
4. Confirm samples are reaching projectM.
5. Confirm the preset path is valid.
6. Reduce complexity before adding more logs or custom GL inspection.

## Deferred features policy

The following are deferred until a post-v2 design note exists:

- Custom post-processing after projectM.
- Injected GLSL overlays.
- Manual FBO chains outside projectM.
- Shader introspection layers.

## Agent rules

Agents should not reintroduce deleted custom GL/GLSL architecture unless the product direction explicitly changes. If a future feature seems to require custom passes, document the need first and obtain approval before implementation.
