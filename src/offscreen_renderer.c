#include "offscreen_renderer.h"

#if HAVE_PROJECTM

#include <SDL2/SDL.h>
#include <epoxy/gl.h>

struct _OffscreenRenderer {
    SDL_Window*   window;
    SDL_GLContext gl_context;
    GLuint        render_fbo;
    GLuint        render_tex;
    int           width;
    int           height;
    gboolean      verbose;
    guint8*       readback_buffer;
    gsize         readback_buffer_size;
};

static gboolean sdl_video_ready = FALSE;

static gboolean
offscreen_renderer_create_target(OffscreenRenderer* renderer,
                                 int                width,
                                 int                height)
{
    if (!renderer || width <= 0 || height <= 0)
        return FALSE;

    if (renderer->render_fbo != 0) {
        glDeleteFramebuffers(1, &renderer->render_fbo);
        renderer->render_fbo = 0;
    }

    if (renderer->render_tex != 0) {
        glDeleteTextures(1, &renderer->render_tex);
        renderer->render_tex = 0;
    }

    glGenFramebuffers(1, &renderer->render_fbo);
    glGenTextures(1, &renderer->render_tex);

    if (renderer->verbose) {
        g_message("offscreen: created FBO=%u, tex=%u for %dx%d",
                  renderer->render_fbo, renderer->render_tex, width, height);
    }

    glBindTexture(GL_TEXTURE_2D, renderer->render_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glBindFramebuffer(GL_FRAMEBUFFER, renderer->render_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           renderer->render_tex,
                           0);

    {
        GLuint fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
            g_warning("offscreen: FBO incomplete for %dx%d (status=0x%04x)",
                      width, height, fbo_status);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return FALSE;
        }
        if (renderer->verbose) {
            g_message("offscreen: FBO complete for %dx%d", width, height);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    renderer->width = width;
    renderer->height = height;
    return TRUE;
}

gboolean
offscreen_renderer_preinit(void)
{
    if (sdl_video_ready)
        return TRUE;

    /* When spawned via Meta.WaylandClient, Mutter injects WAYLAND_SOCKET —
     * a private socketpair fd for the child's exclusive Wayland connection.
     * GTK4's GDK consumes this fd during GtkApplication activation.  If SDL2
     * also reads WAYLAND_SOCKET, its connection attempt fails (one-shot pair,
     * not a listen socket) and SDL_Init(SDL_INIT_VIDEO) aborts on a pure
     * Wayland session.  Unsetting the variable lets SDL2 fall back to the
     * public WAYLAND_DISPLAY socket, which accepts multiple clients.
     * This must happen before SDL_Init() because SDL reads the env at init. */
    g_unsetenv("WAYLAND_SOCKET");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        g_warning("offscreen: SDL_Init falhou: %s", SDL_GetError());
        return FALSE;
    }

    sdl_video_ready = TRUE;
    return TRUE;
}

void
offscreen_renderer_global_shutdown(void)
{
    if (!sdl_video_ready)
        return;

    SDL_Quit();
    sdl_video_ready = FALSE;
}

OffscreenRenderer*
offscreen_renderer_new(void)
{
    return g_new0(OffscreenRenderer, 1);
}

void
offscreen_renderer_free(OffscreenRenderer* renderer)
{
    if (!renderer)
        return;

    offscreen_renderer_shutdown(renderer);
    g_free(renderer);
}

gboolean
offscreen_renderer_init(OffscreenRenderer* renderer,
                        int                width,
                        int                height,
                        gboolean           verbose)
{
    if (!renderer)
        return FALSE;

    if (!offscreen_renderer_preinit())
        return FALSE;

    renderer->verbose = verbose;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    /* Use COMPATIBILITY profile for projectM which uses deprecated GL functions */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");

    renderer->window = SDL_CreateWindow("milkdrop offscreen",
                                        SDL_WINDOWPOS_UNDEFINED,
                                        SDL_WINDOWPOS_UNDEFINED,
                                        MAX(width, 1),
                                        MAX(height, 1),
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!renderer->window) {
        g_warning("offscreen: SDL_CreateWindow falhou: %s", SDL_GetError());
        return FALSE;
    }

    renderer->gl_context = SDL_GL_CreateContext(renderer->window);
    if (!renderer->gl_context) {
        g_warning("offscreen: SDL_GL_CreateContext falhou: %s", SDL_GetError());
        offscreen_renderer_shutdown(renderer);
        return FALSE;
    }

    if (!offscreen_renderer_make_current(renderer)) {
        offscreen_renderer_shutdown(renderer);
        return FALSE;
    }

    /* Disable Mesa's parallel (background) shader compilation so that glFinish()
     * after every projectm_opengl_render_frame_fbo() is a hard synchronization
     * point.  Without this, Mesa's Gallium compiler threads continue running
     * after glFinish() returns, and calling projectm_load_preset_file() while
     * those threads are active dereferences NULL inside the shader compiler
     * (confirmed SIGSEGV in libgallium).  Synchronous compilation costs a few
     * extra milliseconds on the first frame but eliminates the race entirely. */
    if (epoxy_has_gl_extension("GL_KHR_parallel_shader_compile"))
        glMaxShaderCompilerThreadsKHR(0);
    else if (epoxy_has_gl_extension("GL_ARB_parallel_shader_compile"))
        glMaxShaderCompilerThreadsARB(0);

    if (!offscreen_renderer_create_target(renderer, MAX(width, 1), MAX(height, 1))) {
        offscreen_renderer_shutdown(renderer);
        return FALSE;
    }

    return TRUE;
}

void
offscreen_renderer_shutdown(OffscreenRenderer* renderer)
{
    if (!renderer)
        return;

    if (renderer->window && renderer->gl_context)
        SDL_GL_MakeCurrent(renderer->window, renderer->gl_context);

    if (renderer->render_fbo != 0) {
        glDeleteFramebuffers(1, &renderer->render_fbo);
        renderer->render_fbo = 0;
    }

    if (renderer->render_tex != 0) {
        glDeleteTextures(1, &renderer->render_tex);
        renderer->render_tex = 0;
    }

    if (renderer->gl_context) {
        SDL_GL_DeleteContext(renderer->gl_context);
        renderer->gl_context = NULL;
    }

    if (renderer->window) {
        SDL_DestroyWindow(renderer->window);
        renderer->window = NULL;
    }

    g_clear_pointer(&renderer->readback_buffer, g_free);
    renderer->readback_buffer_size = 0;
    renderer->width = 0;
    renderer->height = 0;
}

gboolean
offscreen_renderer_make_current(OffscreenRenderer* renderer)
{
    if (!renderer || !renderer->window || !renderer->gl_context)
        return FALSE;

    if (SDL_GL_MakeCurrent(renderer->window, renderer->gl_context) != 0) {
        g_warning("offscreen: SDL_GL_MakeCurrent falhou: %s", SDL_GetError());
        return FALSE;
    }

    return TRUE;
}

void*
offscreen_renderer_gl_load_proc(const char* name,
                                void*       user_data)
{
    (void)user_data;
    return (void*)SDL_GL_GetProcAddress(name);
}

gboolean
offscreen_renderer_begin_frame(OffscreenRenderer* renderer,
                               int                width,
                               int                height,
                               uint32_t*          out_fbo)
{
    if (!renderer || !out_fbo || width <= 0 || height <= 0)
        return FALSE;

    if (!offscreen_renderer_make_current(renderer))
        return FALSE;

    if (renderer->width != width || renderer->height != height) {
        if (!offscreen_renderer_create_target(renderer, width, height))
            return FALSE;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, renderer->render_fbo);
    glViewport(0, 0, renderer->width, renderer->height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    *out_fbo = (uint32_t)renderer->render_fbo;
    return TRUE;
}

void
offscreen_renderer_finish_gpu(OffscreenRenderer* renderer)
{
    if (!renderer)
        return;

    /* Restore GL state that projectM may have altered. This mirrors the state
     * restoration required by GtkGLArea's render contract and ensures the SDL2
     * GL context is left in a clean state between frames. */
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);

    /* Block CPU until all projectM multi-pass blur commands complete on the GPU.
     * Without this, pixel readback in offscreen_renderer_read_rgba() may capture
     * an intermediate blur pass instead of the final frame. */
    glFinish();
}

gboolean
offscreen_renderer_read_rgba(OffscreenRenderer* renderer,
                             guint8**           out_pixels,
                             gsize*             out_len,
                             gsize*             out_stride)
{
    guint8* pixels = NULL;
    guint8* tmp = NULL;
    gsize stride = 0;
    gsize total = 0;

    if (!renderer || !out_pixels || !out_len || !out_stride ||
        renderer->width <= 0 || renderer->height <= 0) {
        return FALSE;
    }

    if (renderer->render_tex == 0) {
        if (renderer->verbose) {
            g_warning("offscreen: render_tex is 0 (not initialized)");
        }
        return FALSE;
    }

    stride = (gsize)renderer->width * 4u;
    total = stride * (gsize)renderer->height;

    /* Reuse readback buffer when size matches; reduces per-frame allocation. */
    if (renderer->readback_buffer && renderer->readback_buffer_size == total) {
        pixels = renderer->readback_buffer;
    } else {
        g_clear_pointer(&renderer->readback_buffer, g_free);
        renderer->readback_buffer_size = 0;
        pixels = g_malloc(total);
        if (!pixels) {
            return FALSE;
        }
        renderer->readback_buffer = pixels;
        renderer->readback_buffer_size = total;
    }

    /* Re-ensure our SDL2 GL context is current: projectm_opengl_render_frame_fbo
     * may call glXMakeCurrent or other context APIs that displace it. */
    if (!offscreen_renderer_make_current(renderer)) {
        return FALSE;
    }

    /* Use glReadPixels instead of glGetTexImage.
     * glGetTexImage fails with GL_INVALID_OPERATION (0x0502) when GTK4's
     * GSK_RENDERER=gl is active in the same process (Mesa driver conflict
     * between GTK4's GL context and SDL2's GL context).  glReadPixels reads
     * from the bound framebuffer and does not have this conflict.
     *
     * projectM's pipeline may detach GL_COLOR_ATTACHMENT0 from render_fbo
     * during preset transitions; reattach before reading to ensure the FBO
     * is complete. */
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->render_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           renderer->render_tex,
                           0);
    {
        GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
            g_warning("offscreen: FBO incomplete before read (status=0x%04x), "
                      "tex=%u, fbo=%u, dims=%dx%d",
                      fbo_status, renderer->render_tex, renderer->render_fbo,
                      renderer->width, renderer->height);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return FALSE;
        }
    }
    /* Drain deferred errors with a safety limit to prevent infinite loops. */
    for (int i = 0; i < 100; i++) {
        if (glGetError() == GL_NO_ERROR)
            break;
    }

    if (renderer->verbose) {
        g_message("offscreen: pre-read bound FBO=%u, tex=%u, dims=%dx%d",
                  renderer->render_fbo, renderer->render_tex,
                  renderer->width, renderer->height);
    }

    glReadPixels(0, 0, renderer->width, renderer->height,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            g_warning("offscreen: glReadPixels failed (GL error 0x%04x), "
                      "tex=%u, fbo=%u, dims=%dx%d",
                      err, renderer->render_tex, renderer->render_fbo,
                      renderer->width, renderer->height);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return FALSE;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Flip Y in-place: OpenGL origin is bottom-left, GDK expects top-left.
     * Swap rows pairwise using a temporary row buffer to avoid a second
     * full-frame allocation. */
    tmp = g_alloca(stride);
    {
        int half_height = renderer->height / 2;
        for (int y = 0; y < half_height; y++) {
            guint8* top = pixels + (gsize)y * stride;
            guint8* bottom = pixels + (gsize)(renderer->height - 1 - y) * stride;
            memcpy(tmp, top, stride);
            memcpy(top, bottom, stride);
            memcpy(bottom, tmp, stride);
        }
    }

    *out_pixels = pixels;
    *out_len = total;
    *out_stride = stride;
    return TRUE;
}

#else

struct _OffscreenRenderer {
    int unused;
};

gboolean offscreen_renderer_preinit(void) { return FALSE; }
void offscreen_renderer_global_shutdown(void) { }
OffscreenRenderer* offscreen_renderer_new(void) { return NULL; }
void offscreen_renderer_free(OffscreenRenderer* renderer) { g_free(renderer); }
gboolean offscreen_renderer_init(OffscreenRenderer* renderer, int width, int height, gboolean verbose)
{
    (void)renderer; (void)width; (void)height; (void)verbose;
    return FALSE;
}
void offscreen_renderer_shutdown(OffscreenRenderer* renderer) { (void)renderer; }
gboolean offscreen_renderer_make_current(OffscreenRenderer* renderer)
{
    (void)renderer;
    return FALSE;
}
void* offscreen_renderer_gl_load_proc(const char* name, void* user_data)
{
    (void)name; (void)user_data;
    return NULL;
}
gboolean offscreen_renderer_begin_frame(OffscreenRenderer* renderer, int width, int height, uint32_t* out_fbo)
{
    (void)renderer; (void)width; (void)height; (void)out_fbo;
    return FALSE;
}
void offscreen_renderer_finish_gpu(OffscreenRenderer* renderer) { (void)renderer; }
gboolean offscreen_renderer_read_rgba(OffscreenRenderer* renderer, guint8** out_pixels, gsize* out_len, gsize* out_stride)
{
    (void)renderer; (void)out_pixels; (void)out_len; (void)out_stride;
    return FALSE;
}

#endif
