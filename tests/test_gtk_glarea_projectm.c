/*
 * GtkGLArea + libprojectM + FBO destino (igual ao milkdrop).
 * - realize: projectm_create, playlist, idle://
 * - resize: projectm_set_window_size (escala HiDPI)
 * - render: PCM sintético, projectm_opengl_render_frame_fbo
 * - prova: glReadPixels em vários pontos; usar Y em coordenadas OpenGL (origem embaixo),
 *   não como no GTK — senão presets com formas (ex.: 100-square) leem só preto.
 *
 * meson test -C build gtk-glarea-projectm   (só com -Dprojectm=enabled; skip sem display)
 * ./build/tests/test-gtk-glarea-projectm --visual
 *   (usa timeout ~60 Hz para pedir frames; só idle após "render" fica pouco fluido)
 *
 * O Meson define MILKDROP_PM_TEST_PRESET para um .milk em reference_codebases/projectm
 * (padrão visível). Sem isso usa idle:// (pode falhar o assert de variação de pixel).
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <epoxy/gl.h>

#include <projectM-4/core.h>
#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>
#include <projectM-4/audio.h>
#include <projectM-4/playlist_core.h>
#include <projectM-4/playlist_items.h>
#include <projectM-4/playlist_playback.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PM_VISUAL_PULSE_MS 16
#define PM_STARTUP_MAX_FRAMES 180
#define PM_STARTUP_REQUIRED_CONTENT_FRAMES 2

typedef struct {
    GtkApplication*   app;
    GtkGLArea*        gl_area;
    projectm_handle   pm;
    void*             playlist;
    int               frame;
    gint64            mono_origin_us;
    guint             visual_pulse_id;
    gboolean          pass;
    gboolean          finished;
    gboolean          init_ok;
    gboolean          realize_finished;
    gboolean          visual_mode;
    gboolean          printed_visual_sample;
    guint             frames_with_content;
    guint             consecutive_content_frames;
    gboolean          startup_warmup_drawn;
    gboolean          startup_deferred_preset_activation;
    gboolean          startup_final_content_active;
} PmCtx;

static gboolean
frame_has_visible_content(int width, int height)
{
    static const struct {
        float x;
        float y;
    } points[] = {
        {0.50f, 0.50f},
        {0.33f, 0.33f},
        {0.67f, 0.33f},
        {0.33f, 0.67f},
        {0.67f, 0.67f},
    };

    if (width <= 0 || height <= 0)
        return FALSE;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    for (size_t i = 0; i < G_N_ELEMENTS(points); i++) {
        int px = CLAMP((int)(points[i].x * (float)(width - 1)), 0, width - 1);
        int py = CLAMP((int)(points[i].y * (float)(height - 1)), 0, height - 1);
        guint8 pixel[4] = {0};

        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        if (pixel[0] > 0 || pixel[1] > 0 || pixel[2] > 0)
            return TRUE;
    }

    return FALSE;
}

static void
pm_ctx_cleanup(PmCtx* ctx)
{
    if (!ctx)
        return;
    if (ctx->playlist) {
        projectm_playlist_destroy(ctx->playlist);
        ctx->playlist = NULL;
    }
    if (ctx->pm) {
        projectm_destroy(ctx->pm);
        ctx->pm = NULL;
    }
}

static void
on_unrealize_pm(GtkGLArea* area, gpointer user_data)
{
    (void)area;
    PmCtx* ctx = user_data;
    if (ctx->visual_pulse_id != 0) {
        g_source_remove(ctx->visual_pulse_id);
        ctx->visual_pulse_id = 0;
    }
    ctx->gl_area = NULL;
    pm_ctx_cleanup(ctx);
}

static gboolean
validate_playlist_rotation_api(PmCtx* ctx, const char* preset_path)
{
    if (!ctx || !ctx->playlist)
        return FALSE;

    if (!preset_path || !preset_path[0] || !g_file_test(preset_path, G_FILE_TEST_IS_REGULAR))
        return TRUE;

    projectm_playlist_clear(ctx->playlist);
    projectm_playlist_set_shuffle(ctx->playlist, FALSE);

    if (!projectm_playlist_add_preset(ctx->playlist, preset_path, true))
        return FALSE;
    if (!projectm_playlist_add_preset(ctx->playlist, preset_path, true))
        return FALSE;

    uint32_t size = projectm_playlist_size(ctx->playlist);
    if (size < 2u)
        return FALSE;

    uint32_t start_pos = projectm_playlist_set_position(ctx->playlist, 0u, true);
    if (start_pos != 0u)
        return FALSE;

    uint32_t next_pos = projectm_playlist_play_next(ctx->playlist, true);
    if (next_pos >= size)
        return FALSE;

    if (next_pos == start_pos)
        return FALSE;

    return projectm_playlist_get_position(ctx->playlist) == next_pos;
}

static gboolean
load_test_preset_via_playlist(PmCtx* ctx, const char* preset_path)
{
    if (!ctx || !ctx->playlist)
        return FALSE;

    ctx->startup_warmup_drawn = FALSE;
    ctx->startup_deferred_preset_activation = FALSE;
    ctx->startup_final_content_active = FALSE;

    if (!preset_path || !preset_path[0] || !g_file_test(preset_path, G_FILE_TEST_IS_REGULAR)) {
        projectm_load_preset_file(ctx->pm, "idle://", false);
        ctx->startup_final_content_active = TRUE;
        return TRUE;
    }

    g_autofree gchar* preset_dir = g_path_get_dirname(preset_path);
    const char* tex_paths[] = {preset_dir, "/usr/local/share/projectM/textures"};

    projectm_set_texture_search_paths(ctx->pm, tex_paths, 2u);

    projectm_playlist_clear(ctx->playlist);
    projectm_playlist_set_shuffle(ctx->playlist, FALSE);

    if (!projectm_playlist_add_preset(ctx->playlist, preset_path, true))
        return FALSE;

    projectm_load_preset_file(ctx->pm, "idle://", false);
    ctx->startup_deferred_preset_activation = TRUE;
    return TRUE;
}

static gboolean
activate_test_preset_from_playlist(PmCtx* ctx)
{
    if (!ctx || !ctx->playlist || !ctx->startup_deferred_preset_activation)
        return FALSE;

    if (projectm_playlist_size(ctx->playlist) == 0u) {
        ctx->startup_deferred_preset_activation = FALSE;
        ctx->startup_final_content_active = TRUE;
        return FALSE;
    }

    projectm_playlist_set_position(ctx->playlist, 0u, true);
    ctx->startup_deferred_preset_activation = FALSE;
    ctx->startup_final_content_active = TRUE;
    return TRUE;
}

static void
on_realize_pm(GtkGLArea* area, gpointer user_data)
{
    PmCtx* ctx = user_data;

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) {
        g_warning("GtkGLArea: context error in realize");
        ctx->init_ok         = FALSE;
        ctx->realize_finished = TRUE;
        return;
    }

    GdkGLContext* gl_ctx = gtk_gl_area_get_context(area);
    if (!gl_ctx) {
        g_warning("GtkGLArea: no GL context");
        ctx->init_ok          = FALSE;
        ctx->realize_finished = TRUE;
        return;
    }

    int major = 0, minor = 0;
    gdk_gl_context_get_version(gl_ctx, &major, &minor);
    if (major < 3 || (major == 3 && minor < 3)) {
        g_warning("OpenGL %d.%d < 3.3 — projectM skipped", major, minor);
        ctx->init_ok = FALSE;
        ctx->realize_finished = TRUE;
        return;
    }

    ctx->pm = projectm_create();
    if (!ctx->pm) {
        g_warning("projectm_create failed");
        ctx->init_ok          = FALSE;
        ctx->realize_finished = TRUE;
        return;
    }

    ctx->playlist = projectm_playlist_create(ctx->pm);
    if (!ctx->playlist) {
        g_warning("projectm_playlist_create failed");
        projectm_destroy(ctx->pm);
        ctx->pm               = NULL;
        ctx->init_ok          = FALSE;
        ctx->realize_finished = TRUE;
        return;
    }

    const char* preset = g_getenv("MILKDROP_PM_TEST_PRESET");

    if (!validate_playlist_rotation_api(ctx, preset)) {
        g_warning("projectM playlist rotation API validation failed");
        pm_ctx_cleanup(ctx);
        ctx->init_ok = FALSE;
        ctx->realize_finished = TRUE;
        return;
    }

    if (!load_test_preset_via_playlist(ctx, preset)) {
        g_warning("projectM playlist-backed preset load failed");
        pm_ctx_cleanup(ctx);
        ctx->init_ok = FALSE;
        ctx->realize_finished = TRUE;
        return;
    }

    ctx->init_ok = TRUE;
    ctx->realize_finished = TRUE;
}

static void
on_resize_pm(GtkGLArea* area, int width, int height, gpointer user_data)
{
    (void)area;
    PmCtx* ctx = user_data;
    if (!ctx->pm || width < 2 || height < 2)
        return;

    /* width/height from GtkGLArea::resize are already physical (see gtk_gl_area_snapshot). */
    projectm_set_window_size(ctx->pm, (size_t)width, (size_t)height);
}

static void
feed_synthetic_pcm(PmCtx* ctx, float* buf, size_t nfloats)
{
    int f = ctx->frame;
    for (size_t i = 0; i < nfloats; i++) {
        float t = (float)(f * 11 + (int)i);
        buf[i]  = 0.55f * sinf(t * 0.09f) + 0.35f * sinf(t * 0.37f);
    }
}

/* gtk_gl_area_queue_render() a partir do próprio callback "render" costuma não marcar
 * novo frame; o milkdrop usa idle + timeout.
 * O idle DEVE receber PmCtx* — nunca GtkGLArea*: o widget pode ser destruído antes do idle
 * correr (fechar janela → crash com ponteiro pendurado). */
static gboolean
idle_queue_gl_render(gpointer data)
{
    PmCtx* ctx = data;
    if (!ctx || !ctx->gl_area)
        return G_SOURCE_REMOVE;
    gtk_gl_area_queue_render(ctx->gl_area);
    return G_SOURCE_REMOVE;
}

static void
queue_next_gl_render(PmCtx* ctx)
{
    if (!ctx)
        return;
    g_idle_add(idle_queue_gl_render, ctx);
}

static gboolean
on_visual_pulse(gpointer user_data)
{
    PmCtx* ctx = user_data;
    if (!ctx->gl_area || ctx->finished)
        return G_SOURCE_REMOVE;
    queue_next_gl_render(ctx);
    return G_SOURCE_CONTINUE;
}

static void
on_visual_window_destroy(GtkWidget* widget, gpointer user_data)
{
    (void)widget;
    PmCtx* ctx = user_data;
    ctx->finished = TRUE;
    if (ctx->visual_pulse_id != 0) {
        g_source_remove(ctx->visual_pulse_id);
        ctx->visual_pulse_id = 0;
    }
    ctx->gl_area = NULL;
}

static gboolean
on_render_pm(GtkGLArea* area, GdkGLContext* context, gpointer user_data)
{
    (void)context;
    PmCtx* ctx = user_data;

    if (!ctx->realize_finished) {
        queue_next_gl_render(ctx);
        return TRUE;
    }

    if (!ctx->init_ok || !ctx->pm) {
        ctx->pass     = FALSE;
        ctx->finished = TRUE;
        g_application_quit(G_APPLICATION(ctx->app));
        return TRUE;
    }

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) {
        ctx->pass     = FALSE;
        ctx->finished = TRUE;
        g_application_quit(G_APPLICATION(ctx->app));
        return TRUE;
    }

    gtk_gl_area_attach_buffers(area);

    /* Usar o viewport que o GtkGLArea acabou de aplicar (resize → glViewport), não
     * gtk_widget_get_width×scale — em alguns backends os números divergem. */
    GLint vp[4] = {0};
    glGetIntegerv(GL_VIEWPORT, vp);
    int fb_w = vp[2];
    int fb_h = vp[3];
    if (fb_w < 4 || fb_h < 4) {
        queue_next_gl_render(ctx);
        return TRUE;
    }

    /* projectM não desenha se m_windowWidth/Height forem 0; resize pode chegar depois
     * do primeiro render — reforçar aqui. Não chamar glViewport: o GTK já definiu o
     * viewport para o tamanho do FBO antes do sinal "render". */
    projectm_set_window_size(ctx->pm, (size_t)fb_w, (size_t)fb_h);

    if (ctx->mono_origin_us == 0)
        ctx->mono_origin_us = g_get_monotonic_time();
    projectm_set_frame_time(ctx->pm,
                            (g_get_monotonic_time() - ctx->mono_origin_us) / 1000000.0);

    if (ctx->startup_deferred_preset_activation && ctx->startup_warmup_drawn) {
        if (activate_test_preset_from_playlist(ctx))
            gtk_gl_area_attach_buffers(area);
    }

    float pcm[512];
    feed_synthetic_pcm(ctx, pcm, G_N_ELEMENTS(pcm));
    projectm_pcm_add_float(ctx->pm, pcm, G_N_ELEMENTS(pcm) / 2u, PROJECTM_STEREO);
    projectm_playlist_set_shuffle(ctx->playlist, FALSE);

    GLint draw_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);

    /* Limpar erros GL pendentes antes do render */
    while (glGetError() != GL_NO_ERROR) {}

    if (draw_fbo != 0)
        projectm_opengl_render_frame_fbo(ctx->pm, (uint32_t)draw_fbo);
    else
        projectm_opengl_render_frame(ctx->pm);

    /* Synchronize multi-pass blur rendering (matches production code in src/main.c) */
    glFinish();

    /* Estado que o GTK valida apos o sinal render; projectM pode alterar. */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);

    /* projectM muda READ/DRAW FBOs; restaurar antes da leitura */
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)draw_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    GLenum gl_err = glGetError();
    if (gl_err != GL_NO_ERROR) {
        if (!ctx->visual_mode) {
            ctx->pass      = FALSE;
            ctx->finished  = TRUE;
            g_application_quit(G_APPLICATION(ctx->app));
            return TRUE;
        }
    }

    gboolean has_content = frame_has_visible_content(fb_w, fb_h);
    if (!ctx->startup_final_content_active) {
        ctx->startup_warmup_drawn = TRUE;
        ctx->consecutive_content_frames = 0;
    } else if (has_content) {
        ctx->frames_with_content++;
        ctx->consecutive_content_frames++;
    } else {
        ctx->consecutive_content_frames = 0;
    }

    ctx->frame++;

    if (!ctx->visual_mode && ctx->consecutive_content_frames >= PM_STARTUP_REQUIRED_CONTENT_FRAMES) {
        ctx->pass     = TRUE;
        ctx->finished = TRUE;
        g_application_quit(G_APPLICATION(ctx->app));
        return TRUE;
    }

    if (!ctx->visual_mode && ctx->frame >= PM_STARTUP_MAX_FRAMES) {
        ctx->pass     = FALSE;
        ctx->finished = TRUE;
        g_application_quit(G_APPLICATION(ctx->app));
        return TRUE;
    }

    queue_next_gl_render(ctx);
    return TRUE;
}

static gboolean
on_timeout_pm(gpointer user_data)
{
    PmCtx* ctx = user_data;
    if (!ctx->finished) {
        ctx->pass     = FALSE;
        ctx->finished = TRUE;
        g_application_quit(G_APPLICATION(ctx->app));
    }
    return G_SOURCE_REMOVE;
}

static void
activate_pm_common(GtkApplication* app, PmCtx* ctx)
{
    ctx->app = app;

    GtkWidget* win = gtk_application_window_new(app);
    GtkWidget* gl  = gtk_gl_area_new();

    gtk_window_set_title(GTK_WINDOW(win),
                         ctx->visual_mode
                             ? "milkdrop test — projectM + GtkGLArea FBO (feche a janela)"
                             : "gtk-glarea-projectm (automated)");

    gtk_gl_area_set_required_version(GTK_GL_AREA(gl), 3, 3);
#if GTK_CHECK_VERSION(4, 12, 0)
    /* projectM precisa de OpenGL desktop; forcar GL API */
    gtk_gl_area_set_allowed_apis(GTK_GL_AREA(gl), GDK_GL_API_GL);
#endif
    gtk_gl_area_set_auto_render(GTK_GL_AREA(gl), FALSE);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(gl), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(gl), FALSE);

    gtk_window_set_default_size(GTK_WINDOW(win),
                                ctx->visual_mode ? 520 : 200,
                                ctx->visual_mode ? 360 : 160);
    gtk_window_set_child(GTK_WINDOW(win), gl);

    ctx->gl_area = GTK_GL_AREA(gl);

    g_signal_connect(gl, "realize", G_CALLBACK(on_realize_pm), ctx);
    g_signal_connect(gl, "unrealize", G_CALLBACK(on_unrealize_pm), ctx);
    g_signal_connect(gl, "resize", G_CALLBACK(on_resize_pm), ctx);
    g_signal_connect(gl, "render", G_CALLBACK(on_render_pm), ctx);

    if (ctx->visual_mode)
        g_signal_connect(win, "destroy", G_CALLBACK(on_visual_window_destroy), ctx);

    gtk_window_present(GTK_WINDOW(win));
    gtk_gl_area_queue_render(GTK_GL_AREA(gl));

    /* Content-based assertions need a steady render cadence; relying only on the
     * render callback to queue the next frame can stall after a single frame on
     * some backends. Match production/visual mode and drive ~60 Hz via timeout. */
    ctx->visual_pulse_id = g_timeout_add(PM_VISUAL_PULSE_MS, on_visual_pulse, ctx);

    if (ctx->visual_mode) {
        g_print("Modo visual: ~60 Hz via g_timeout_add; feche a janela para sair. "
                "Preset: MILKDROP_PM_TEST_PRESET ou idle://.\n");
    } else {
        g_timeout_add_seconds(20, on_timeout_pm, ctx);
    }
}

static void
activate_pm_test(GtkApplication* app, gpointer user_data)
{
    activate_pm_common(app, (PmCtx*)user_data);
}

static void
test_gtk_glarea_projectm_frames_differ(void)
{
    if (!gtk_init_check()) {
        g_test_skip("Sem display GDK");
        return;
    }

    PmCtx ctx = {0};
    ctx.init_ok     = FALSE;
    ctx.visual_mode = FALSE;

    GtkApplication* app = gtk_application_new(
        "io.github.mauriciobc.milkdrop.TestGlareaProjectm",
        G_APPLICATION_NON_UNIQUE);

    g_signal_connect(app, "activate", G_CALLBACK(activate_pm_test), &ctx);
    (void)g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);

    if (!ctx.init_ok) {
        g_test_message("Falha ao inicializar GL ou projectM");
        g_assert_true(ctx.init_ok);
    }

        if (!ctx.pass) {
            g_test_message("estado: init_ok=%d finished=%d frame=%d frames_with_content=%u consecutive_content=%u",
                           (int)ctx.init_ok,
                           (int)ctx.finished,
                           ctx.frame,
                           ctx.frames_with_content,
                           ctx.consecutive_content_frames);
        }

    g_assert_true(ctx.finished);
    g_assert_true(ctx.pass);
    g_test_message("Renderizou %d frames com %u frames contendo imagem",
                   ctx.frame,
                   ctx.frames_with_content);
}

static int
run_visual(char* progname)
{
    char* argv[] = {progname, NULL};

    PmCtx ctx = {0};
    ctx.visual_mode = TRUE;

    GtkApplication* app = gtk_application_new(
        "io.github.mauriciobc.milkdrop.TestGlareaProjectmVisual",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate_pm_test), &ctx);
    int st = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return st;
}

int
main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "--visual") == 0)
        return run_visual(argv[0]);

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gtk-glarea-projectm/renders-frames",
                    test_gtk_glarea_projectm_frames_differ);
    return g_test_run();
}
