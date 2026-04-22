# EGL Image, DMA-BUF, and Cross-Context Texture Sharing in GTK4

## Purpose

This document explains why the SDL2 offscreen renderer cannot share textures directly with
the GTK4 GtkGLArea context, and establishes the `glReadPixels` + CPU-upload pattern as the
correct and portable fallback.

## EGL Image and DMA-BUF Fundamentals

**EGL_KHR_image_base** defines `EGLImage` as an opaque handle to shared 2D image data that
EGL can create from client API resources (GL texture objects, renderbuffers, etc.).

**EGL_EXT_image_dma_buf_import** extends this by allowing creation of an `EGLImage` from a
Linux `dma_buf` file descriptor, enabling zero-copy texture import for multi-plane YUV and
RGB buffers. The workflow:

```
eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs)
  → EGLImage handle
glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, egl_image)
  → image bound as texture in the calling context
```

EGL holds a reference on the `dma_buf`; the image is valid until `eglTerminate()`. This is
the foundation GTK4 uses internally for zero-copy Wayland surface compositing.

## GTK4 GdkGLContext Architecture on EGL/Wayland

`GdkGLContext` is GTK's cross-platform abstraction over native GL contexts. On Wayland it
uses EGL exclusively (GTK4 migrated the X11 backend from GLX to EGL to unify the texture
import path).

**Context sharing model:** GTK manages an implicit "master" context for each display. All
`GdkGLContext` instances for the same display inherit resource sharing from that parent.
The public API does **not** allow specifying an external shared context — sharing is
internal to GTK and not extensible from application code.

`gdk_gl_context_is_shared(ctx1, ctx2)` returns `TRUE` if two realized contexts share the
same resource namespace. Both must be realized before calling it; unrealized contexts
always return `FALSE`.

**`use_es` property:** `gdk_gl_context_set_use_es()` / `gtk_gl_area_set_use_es()` request
OpenGL ES instead of desktop GL. Must be called **before** context realization; calling it
after realization is a no-op.

## GtkGLArea: Signals, FBO Lifecycle, and Constraints

| Signal | When | Notes |
|--------|------|-------|
| `create-context` | During widget realization | App can return a custom context; default creates one via GTK |
| `realize` (inherited) | After context created | Initialize GL resources here, context is already current |
| `unrealize` (inherited) | Before context destroyed | Free all GL resources; GTK tears down its FBO after this |
| `render` | Every frame | GTK binds its own internal FBO **before** firing this signal |
| `resize` | On dimension changes | Also fires once at realize |

**Critical constraint:** GTK creates and manages its own FBO. The app **cannot** supply a
replacement FBO. All drawing during `render` must target GTK's FBO. GTK then composites
that FBO texture into the Clutter scene graph. This means cross-context texture sharing via
`glBindTexture` alone is impossible — the FBO texture lives in GTK's context, not the
application's secondary context.

## Why Cross-Context Sharing Between SDL2 and GTK4 Is Unreliable

Even with nominally shared EGL contexts, texture objects are not reliably interoperable
across contexts created by separate libraries:

1. **No public shared-context API in SDL2.** `SDL_GL_CreateContext()` creates a context
   independently. SDL2 has no public parameter to specify a share group with an existing
   EGL context. Enabling sharing would require calling EGL directly before SDL initialization.

2. **Per-context texture namespaces.** Many GL drivers maintain per-context object
   namespaces. A texture name created in SDL's context may be invalid or alias different
   storage when referenced in GTK's context.

3. **EGL image attachment is context-local.** When you call
   `glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image)`, the resulting binding
   belongs to the current context. Re-binding that texture name in a different context
   does not transfer the EGL image attachment.

4. **NVIDIA driver regression.** Driver version 595.45.04+ introduced `GL_INVALID_OPERATION`
   errors from `glEGLImageTargetTexture2DOES` with dma_buf images despite correct format
   attributes. Workaround: downgrade to 590.48.01 or avoid the dma_buf path entirely.

5. **DRM fourcc mismatch.** `eglCreateImageKHR` with `EGL_LINUX_DMA_BUF_EXT` requires a
   correct DRM fourcc format code (e.g., `DRM_FORMAT_XRGB8888`), not a GL internal format
   enum. Mismatch produces `EGL_BAD_PARAMETER` or `EGL_BAD_ALLOC` silently.

## Safe Fallback: glReadPixels + CPU Upload

This is the portable, driver-agnostic pattern and is what `offscreen_renderer_read_rgba()`
implements:

```
SDL context active
  glBindFramebuffer(GL_READ_FRAMEBUFFER, sdl_render_fbo)
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, cpu_buffer)
  glFinish()   ← ensures GPU writes complete before CPU reads

gtk_gl_area_make_current()   ← GTK context now active
  glBindTexture(GL_TEXTURE_2D, display_tex)
  glTexImage2D(...)   ← upload cpu_buffer to GTK-local texture
  use texture in render signal callback
```

**Synchronization requirement:** `glFinish()` must be called in the SDL context before
touching `cpu_buffer` on the CPU. Different drivers have different implicit pipeline
synchronization; `glFinish()` is conservative but correct.

**Performance trade-off:** `glReadPixels` stalls the GPU pipeline and involves a PCIe
transfer. For a full-screen 4K HiDPI surface this is significant. Acceptable mitigations:

- Render at the monitor's logical resolution (not physical 4K), letting the compositor
  scale; the visualizer content does not require pixel-perfect HiDPI.
- Accept 1-frame display latency — the visualizer is audio-driven, not input-driven.
- If the platform reliably supports dma_buf import (Mesa + Intel/AMD), a zero-copy path
  can be conditionally enabled at runtime using `gdk_display_get_dmabuf_formats()` (GTK
  4.14+); fall back to CPU upload otherwise.

## Relevant API Reference

### GdkGLContext (`gdk4/gdk-gl-context.h`)

```c
GdkGLContext *gdk_surface_create_gl_context(GdkSurface *surface, GError **error);
void          gdk_gl_context_make_current(GdkGLContext *context);
void          gdk_gl_context_clear_current(void);
gboolean      gdk_gl_context_is_shared(GdkGLContext *self, GdkGLContext *other);
gboolean      gdk_gl_context_realize(GdkGLContext *context, GError **error);
void          gdk_gl_context_set_use_es(GdkGLContext *context, int use_es);
```

### GtkGLArea (`gtk4/gtkglarea.h`)

```c
void          gtk_gl_area_make_current(GtkGLArea *area);
GError       *gtk_gl_area_get_error(GtkGLArea *area);
void          gtk_gl_area_set_use_es(GtkGLArea *area, gboolean use_es);
/* Signals: create-context, render, resize */
```

### EGL extensions

```c
/* EGL_KHR_image_base */
EGLImage eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx,
                            EGLenum target, EGLClientBuffer buffer,
                            const EGLint *attrib_list);
EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImage image);

/* GL_OES_EGL_image */
void glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image);
```

### SDL2 (`SDL2/SDL_video.h`)

```c
SDL_GLContext SDL_GL_CreateContext(SDL_Window *window);
int           SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context);
/* No public parameter for shared context */
```

## Canonical Sources

- GTK Blog — Adventures in Graphics APIs: https://blog.gtk.org/2021/05/10/adventures-in-graphics-apis/
- GtkGLArea docs: https://docs.gtk.org/gtk4/class.GLArea.html
- GdkGLContext docs: https://docs.gtk.org/gdk4/class.GLContext.html
- EGL_KHR_image_base spec: https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_image_base.txt
- EGL_EXT_image_dma_buf_import spec: https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt
- glReadPixels reference: https://docs.gl/gl3/glReadPixels
