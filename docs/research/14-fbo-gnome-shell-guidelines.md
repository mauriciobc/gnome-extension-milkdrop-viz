# FBO Guidelines in the GNOME Shell Stack

## Document Purpose

This document synthesizes official GNOME FBO guidelines gathered from GTK4,
Clutter, and Mutter/Cogl documentation. It defines the correct FBO usage
patterns for each layer of this project.

**Sources consulted (2026-04-21)**:
- https://docs.gtk.org/gtk4/class.GLArea.html
- https://docs.gtk.org/gtk4/method.GLArea.attach_buffers.html
- https://gjs-docs.gnome.org/clutter14~14/clutter.offscreeneffect
- https://mutter.gnome.org/cogl/class.Framebuffer.html

---

## 1. GTK4 `GtkGLArea` — C Renderer Subprocess

### Ownership

`GtkGLArea` creates and owns its own FBO internally. The widget guarantees this
FBO is the bound draw/read target when the `::render` signal fires. Application
code must not assume FBO 0 is the correct target inside a render callback.

### Querying the Active FBO

The canonical GTK4 pattern for accessing the current FBO ID (required before
passing it to third-party renderers like projectM):

```c
GLuint gtk_fbo = 0;
glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, (GLint *)&gtk_fbo);
```

Call this **inside** the `::render` callback, after `gtk_gl_area_attach_buffers()`
has been called. The value will be non-zero when GTK's FBO is active.

### `gtk_gl_area_attach_buffers()`

Ensures the area's FBO is the current draw/read target and creates all required
buffers (depth, stencil if requested via `set_has_depth_buffer` /
`set_has_stencil_buffer`).

- Called **automatically** before `::render` signal — do not call manually in a
  simple render callback.
- **Must be called manually** when returning to GTK's FBO after an intermediate
  render pass that changed the active binding.

### Intermediate FBO Pattern

For render-to-texture passes within a GtkGLArea render callback:

```c
static gboolean on_render(GtkGLArea *area, GdkGLContext *ctx, gpointer data)
{
    /* 1. Save GTK's FBO ID */
    GLuint gtk_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, (GLint *)&gtk_fbo);

    /* 2. Bind intermediate FBO and render offscreen */
    glBindFramebuffer(GL_FRAMEBUFFER, my_intermediate_fbo);
    /* ... draw ... */

    /* 3. Restore GTK's FBO before final output */
    glBindFramebuffer(GL_FRAMEBUFFER, gtk_fbo);
    /* OR: gtk_gl_area_attach_buffers(area); — equivalent */

    /* 4. Composite intermediate result to GTK's FBO */
    /* ... blit or draw final quad ... */
    return TRUE;
}
```

### Never Bind to FBO 0

Do **not** call `glBindFramebuffer(GL_FRAMEBUFFER, 0)` inside a GtkGLArea render
callback. FBO 0 is the window-system default framebuffer, not GTK's internal FBO.
Binding to 0 bypasses GTK's compositing chain and produces incorrect output.

### GL State Restoration After Third-Party Renderers

After calling `projectm_opengl_render_frame_fbo()` (or any third-party renderer),
restore the following state before returning from the render callback:

```c
glUseProgram(0);                   /* unbind shader programs        */
glBindTexture(GL_TEXTURE_2D, 0);   /* unbind textures               */
glActiveTexture(GL_TEXTURE0);      /* reset to texture unit 0       */
glDisable(GL_BLEND);               /* disable blending              */
glDisable(GL_DEPTH_TEST);          /* disable depth test            */
glDisable(GL_STENCIL_TEST);        /* disable stencil test          */
glDisable(GL_SCISSOR_TEST);        /* disable scissor test          */
```

GTK validates these states after the `::render` signal returns. Failure to
restore them causes rendering artifacts or GTK warnings.

### Depth and Stencil Buffers

`gtk_gl_area_set_has_depth_buffer()` and `gtk_gl_area_set_has_stencil_buffer()`
must be set **before realization**. GTK attaches these buffers to its own FBO;
they are not available on user-created FBOs.

---

## 2. SDL2 Offscreen Renderer — This Project's Current Path

This project's C binary uses an SDL2 offscreen renderer (`src/offscreen_renderer.c`)
rather than exposing a GtkGLArea to the GNOME Shell compositor. The SDL2 context
is self-contained, so the `glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING)` pattern is
not needed — the renderer allocates and owns its FBO directly:

```c
/* offscreen_renderer_begin_frame(): allocate/bind, return FBO ID */
glBindFramebuffer(GL_FRAMEBUFFER, renderer->render_fbo);
*out_fbo = (uint32_t)renderer->render_fbo;

/* main.c milkdrop_render_frame(): pass directly to projectM */
projectm_opengl_render_frame_fbo(app_data->projectm, target_fbo);
```

The GtkGLArea intermediate FBO save/restore pattern **does not apply** here; the
SDL2 context is not shared with GTK.

### Frame Wind-Down (`offscreen_renderer_finish_gpu`)

After `projectm_opengl_render_frame_fbo()`, call `offscreen_renderer_finish_gpu()`
which applies the 7-item GL state restoration block and then calls `glFinish()`
to ensure multi-pass blur has fully completed before pixel readback.

### After Readback

After `glReadPixels` in `offscreen_renderer_read_rgba()`, unbind the read
framebuffer so the SDL2 context is not left with a dangling attachment:

```c
glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
```

---

## 3. Clutter `OffscreenEffect` — Shell Process (GJS)

For Shell-side offscreen rendering within GNOME Shell's Clutter scene graph,
the canonical abstraction is `Clutter.OffscreenEffect`:

- Manages offscreen `CoglFramebuffer` creation and lifetime automatically.
- Redirects all actor paint operations into an offscreen buffer.
- Exposes the rendered result as a `Cogl.Texture` via `get_texture()`.
- `paint_target()` composites the texture back to the stage.

**GJS subclassing pattern**:

```js
import Clutter from 'gi://Clutter';

class MyEffect extends Clutter.OffscreenEffect {
    vfunc_paint_target(node, paintCtx) {
        // get_texture() — offscreen result texture (may change after pre_paint)
        // get_pipeline() — Cogl pipeline targeting the texture
        // get_target_size() — current offscreen buffer dimensions
        super.vfunc_paint_target(node, paintCtx);
    }

    vfunc_create_texture(w, h) {
        // Override to allocate a custom-sized or differently-formatted texture.
        return super.vfunc_create_texture(w, h);
    }
}
```

**Rules**:
- Do not call `glBindFramebuffer` or allocate raw GL FBOs from GJS.
- All FBO lifecycle is managed by Clutter; GJS code only overrides paint behavior.
- `get_target_size()` is valid only inside `vfunc_paint_target`.

---

## 4. Mutter Cogl `Cogl.Framebuffer` — Internal Mutter Layer

`Cogl.Framebuffer` is Mutter's internal abstraction used by Clutter effects and
Shell rendering infrastructure. It is **not** directly used in the C renderer
subprocess. Listed here for completeness when extending Shell-side visual effects.

| Method | Purpose |
|--------|---------|
| `cogl_framebuffer_finish()` | Blocks CPU until all GPU commands complete (equivalent to `glFinish()`). Rarely needed; documented as "very rare that developers need this". Use before pixel readback. |
| `cogl_framebuffer_flush()` | Submits the current batch to the GPU without blocking (equivalent to `glFlush()`). |
| `cogl_framebuffer_discard_buffers()` | Declares buffer contents no longer needed. Important optimization for tile-based GPU drivers (avoids unnecessary store operations). |
| `cogl_framebuffer_read_pixels()` | Synchronous pixel readback. Always call `finish()` first. |
| `cogl_framebuffer_set_viewport()` | Cogl maintains per-framebuffer viewport state. |
| `cogl_framebuffer_allocate()` | Explicit allocation with error output. Check return value before use. |

`CoglOffscreen` (concrete subclass of `CoglFramebuffer`) is the offscreen type,
but in practice `Clutter.OffscreenEffect` manages this for you.

---

## 5. Project-Specific Rule Summary

| Context | FBO Rule |
|---------|---------|
| **C renderer, `offscreen_renderer_begin_frame()`** | Bind `render_fbo` directly; return its ID via `out_fbo`. No `glGetIntegerv` needed — SDL2 context is self-contained. |
| **C renderer, after `projectm_opengl_render_frame_fbo()`** | Call `offscreen_renderer_finish_gpu()`: 7-item GL state restore → `glFinish()`. |
| **C renderer, after `glReadPixels`** | Unbind: `glBindFramebuffer(GL_READ_FRAMEBUFFER, 0)`. |
| **GtkGLArea render callback (tests/future)** | Query FBO with `glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING)`. Never bind to FBO 0. Call `gtk_gl_area_attach_buffers()` to restore after intermediate passes. |
| **Shell extension actor effect (GJS)** | Use `Clutter.OffscreenEffect`. Never call `glBindFramebuffer` from GJS. |
| **Depth/stencil with GtkGLArea** | Set `has_depth_buffer` / `has_stencil_buffer` before realize; GTK attaches them to its FBO only. |
| **Cogl Framebuffer (shell C code)** | Use `cogl_framebuffer_finish()` before pixel readback; use `discard_buffers()` when tile data is no longer needed. |

---

## 6. Key Absence

GNOME Shell's public extension API has no mechanism to allocate or manipulate raw
OpenGL FBOs within the shell process. All offscreen work in the shell is expected
to go through `Clutter.OffscreenEffect` / Cogl. Raw OpenGL FBO manipulation is
only appropriate in the **renderer subprocess** (the C binary) with an SDL2 or
GtkGLArea GL context.
