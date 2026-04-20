/*
 * GDK GL context + libprojectM + FBO próprio, sem GtkGLArea.
 * Objetivo: separar "bug do contexto GTK/GDK" de "bug específico do GtkGLArea".
 *
 * Uso:
 *   MILKDROP_PM_TEST_PRESET=/path/to/preset.milk ./build/tests/test-gdk-glcontext-projectm
 */

#include <glib.h>
#include <gtk/gtk.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <projectM-4/core.h>
#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>
#include <projectM-4/audio.h>
#include <projectM-4/playlist_core.h>
#include <projectM-4/playlist_items.h>
#include <projectM-4/playlist_playback.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    projectm_handle          pm;
    projectm_playlist_handle playlist;
    GdkGLContext*            gl_context;
    GtkWidget*               window;
    int                      width;
    int                      height;
} GdkPmCtx;

static void*
gdk_gl_load(const char* name,
            void*       user_data)
{
    (void)user_data;
    void* func = (void*)eglGetProcAddress(name);
    if (!func)
        func = dlsym(RTLD_DEFAULT, name);
    return func;
}

static void
feed_synthetic_pcm(float* buf,
                   size_t nfloats,
                   int    frame_index)
{
    for (size_t i = 0; i < nfloats; i++) {
        float t = (float)(frame_index * 11 + (int)i);
        buf[i]  = 0.55f * sinf(t * 0.09f) + 0.35f * sinf(t * 0.37f);
    }
}

static gboolean
ppm_has_visible_content(const char* path)
{
    g_autofree guint8* data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, (char**)&data, &len, NULL))
        return FALSE;

    guint spaces = 0;
    gsize offset = 0;
    while (offset < len && spaces < 4) {
        if (data[offset++] == '\n')
            spaces++;
    }
    if (offset >= len)
        return FALSE;

    for (gsize i = offset; i + 2 < len; i += 3) {
        if (data[i] || data[i + 1] || data[i + 2])
            return TRUE;
    }
    return FALSE;
}

static gboolean
write_ppm(const char* path,
          int         width,
          int         height)
{
    size_t total = (size_t)width * (size_t)height * 4u;
    g_autofree guint8* pixels = g_malloc0(total);
    FILE* f = NULL;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    while (glGetError() != GL_NO_ERROR) {}
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (glGetError() != GL_NO_ERROR)
        return FALSE;

    f = fopen(path, "wb");
    if (!f)
        return FALSE;

    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            guint8* px = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4u;
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    return TRUE;
}

static gboolean
run_basic_fbo_smoke_test(int width,
                         int height)
{
    GLuint fbo = 0;
    GLuint tex = 0;
    guint8 px[4] = {0};

    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return FALSE;

    glViewport(0, 0, width, height);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    return px[0] > 200 && px[1] < 20 && px[2] > 200;
}

static gboolean
init_projectm(GdkPmCtx*    ctx,
              const char*  preset_path)
{
    g_autofree gchar* preset_dir = NULL;
    const char* tex_paths[2] = {0};

    ctx->pm = projectm_create_with_opengl_load_proc(gdk_gl_load, NULL);
    if (!ctx->pm)
        return FALSE;

    ctx->playlist = projectm_playlist_create(ctx->pm);
    if (!ctx->playlist)
        return FALSE;

    if (preset_path && preset_path[0]) {
        preset_dir = g_path_get_dirname(preset_path);
        tex_paths[0] = preset_dir;
        tex_paths[1] = "/usr/local/share/projectM/textures";
        projectm_set_texture_search_paths(ctx->pm, tex_paths, 2u);

        if (!projectm_playlist_add_preset(ctx->playlist, preset_path, true))
            return FALSE;
        projectm_load_preset_file(ctx->pm, preset_path, false);
    } else {
        projectm_load_preset_file(ctx->pm, "idle://", false);
    }

    projectm_set_window_size(ctx->pm, (size_t)ctx->width, (size_t)ctx->height);
    projectm_set_preset_duration(ctx->pm, 300.0);
    projectm_playlist_set_shuffle(ctx->playlist, FALSE);
    return TRUE;
}

static int
run_case(const char* preset_path)
{
    GError* error = NULL;
    GdkPmCtx ctx = {0};
    ctx.width = 320;
    ctx.height = 240;

    if (!gtk_init_check()) {
        g_printerr("Sem display GDK\n");
        return 77;
    }

    GdkDisplay* display = gdk_display_get_default();
    if (!display) {
        g_printerr("Sem GdkDisplay\n");
        return 1;
    }

    if (!gdk_display_prepare_gl(display, &error)) {
        g_printerr("gdk_display_prepare_gl falhou: %s\n", error ? error->message : "(null)");
        g_clear_error(&error);
        return 1;
    }

    ctx.window = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(ctx.window), ctx.width, ctx.height);
    gtk_widget_realize(ctx.window);

    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(ctx.window));
    if (!surface) {
        g_printerr("GtkWindow nao expôs GdkSurface\n");
        return 1;
    }

    ctx.gl_context = gdk_surface_create_gl_context(surface, &error);
    if (!ctx.gl_context) {
        g_printerr("gdk_surface_create_gl_context falhou: %s\n", error ? error->message : "(null)");
        g_clear_error(&error);
        return 1;
    }

    gdk_gl_context_set_required_version(ctx.gl_context, 3, 3);
#if GTK_CHECK_VERSION(4, 12, 0)
    gdk_gl_context_set_allowed_apis(ctx.gl_context, GDK_GL_API_GL);
#endif
    gdk_gl_context_set_debug_enabled(ctx.gl_context, TRUE);
    if (!gdk_gl_context_realize(ctx.gl_context, &error)) {
        g_printerr("gdk_gl_context_realize falhou: %s\n", error ? error->message : "(null)");
        g_clear_error(&error);
        return 1;
    }

    gdk_gl_context_make_current(ctx.gl_context);

    if (!run_basic_fbo_smoke_test(ctx.width, ctx.height)) {
        g_printerr("Smoke test do FBO no contexto GDK falhou\n");
        return 5;
    }

    if (!init_projectm(&ctx, preset_path)) {
        g_printerr("Falha ao inicializar projectM no contexto GDK\n");
        return 1;
    }

    for (int frame = 0; frame < 48; frame++) {
        float pcm[512 * 2];
        GLuint fbo = 0;
        GLuint tex = 0;

        feed_synthetic_pcm(pcm, G_N_ELEMENTS(pcm), frame);
        projectm_set_frame_time(ctx.pm, (double)frame / 60.0);
        projectm_pcm_add_float(ctx.pm, pcm, 512, PROJECTM_STEREO);

        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ctx.width, ctx.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            g_printerr("FBO do contexto GDK incompleto\n");
            return 1;
        }

        while (glGetError() != GL_NO_ERROR) {}
        projectm_opengl_render_frame_fbo(ctx.pm, fbo);
        glFinish();
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            g_printerr("post-render GL error 0x%x no frame %d\n", err, frame + 1);
            return 2;
        }

        if (frame == 47) {
            const char* ppm = "/tmp/gdk-glcontext-projectm.ppm";
            if (!write_ppm(ppm, ctx.width, ctx.height)) {
                g_printerr("Falha ao gravar PPM do contexto GDK\n");
                return 3;
            }
            if (!ppm_has_visible_content(ppm)) {
                g_printerr("PPM do contexto GDK sem conteudo visivel\n");
                return 4;
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
    }

    projectm_playlist_destroy(ctx.playlist);
    projectm_destroy(ctx.pm);
    g_object_unref(ctx.gl_context);
    gtk_window_destroy(GTK_WINDOW(ctx.window));
    return 0;
}

int
main(void)
{
    const char* preset = g_getenv("MILKDROP_PM_TEST_PRESET");
    return run_case(preset);
}
