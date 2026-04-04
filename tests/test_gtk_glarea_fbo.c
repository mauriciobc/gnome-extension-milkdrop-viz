/*
 * Minimal check that GtkGLArea's internal FBO accepts draws and readback.
 * Same pattern as milkdrop on_render: make_current → attach_buffers → draw → read.
 *
 * Automated:  meson test -C build gtk-glarea-fbo   (skips if no display)
 * Visual:      ./build/tests/test-gtk-glarea-fbo --visual
 */

#include <glib.h>
#include <gtk/gtk.h>
#include <epoxy/gl.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    GtkApplication* app;
    gboolean        finished;
    gboolean        read_ok;
    guint8          rgba[4];
} FboTestCtx;

static gboolean
on_render_test(GtkGLArea* area, GdkGLContext* context, gpointer user_data)
{
    (void)context;
    FboTestCtx* t = user_data;

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area))
        return TRUE;

    gtk_gl_area_attach_buffers(area);

    int w = gtk_widget_get_width(GTK_WIDGET(area));
    int h = gtk_widget_get_height(GTK_WIDGET(area));
    if (w < 4 || h < 4) {
        gtk_gl_area_queue_render(area);
        return TRUE;
    }

    glViewport(0, 0, w, h);
    glClearColor(0.12f, 0.88f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (glGetError() != GL_NO_ERROR) {
        t->finished  = TRUE;
        t->read_ok   = FALSE;
        g_application_quit(G_APPLICATION(t->app));
        return TRUE;
    }

    glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
    if (glGetError() != GL_NO_ERROR) {
        t->finished = TRUE;
        t->read_ok  = FALSE;
        g_application_quit(G_APPLICATION(t->app));
        return TRUE;
    }

    /* Expect strong green from our clear (same FBO GTK composites to the window). */
    t->read_ok  = (t->rgba[1] > 200u && t->rgba[0] < 100u && t->rgba[2] < 100u);
    t->finished = TRUE;
    g_application_quit(G_APPLICATION(t->app));
    return TRUE;
}

static gboolean
on_timeout_stop(gpointer user_data)
{
    FboTestCtx* t = user_data;
    if (!t->finished) {
        t->read_ok  = FALSE;
        t->finished = TRUE;
        g_application_quit(G_APPLICATION(t->app));
    }
    return G_SOURCE_REMOVE;
}

static void
activate_test(GtkApplication* app, gpointer user_data)
{
    FboTestCtx* t   = user_data;
    t->app          = app;
    GtkWidget* win  = gtk_application_window_new(app);
    GtkWidget* gl   = gtk_gl_area_new();

    gtk_window_set_title(GTK_WINDOW(win), "gtk-glarea-fbo (automated)");
    gtk_gl_area_set_required_version(GTK_GL_AREA(gl), 3, 3);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(gl), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 160, 120);
    gtk_window_set_child(GTK_WINDOW(win), gl);
    g_signal_connect(gl, "render", G_CALLBACK(on_render_test), t);
    gtk_window_present(GTK_WINDOW(win));
    gtk_gl_area_queue_render(GTK_GL_AREA(gl));

    g_timeout_add_seconds(5, on_timeout_stop, t);
}

static void
test_glarea_fbo_green_readback(void)
{
    if (!gtk_init_check()) {
        g_test_skip("Sem display GDK (use sessão gráfica ou xvfb-run)");
        return;
    }

    FboTestCtx t = {0};
    GtkApplication* app = gtk_application_new(
        "io.github.mauriciobc.milkdrop.TestGlareaFbo",
        G_APPLICATION_NON_UNIQUE);

    g_signal_connect(app, "activate", G_CALLBACK(activate_test), &t);
    (void)g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);

    if (!t.read_ok) {
        g_test_message("glReadPixels centro: R=%u G=%u B=%u A=%u (esperado G alto, R/B baixos)",
                       (unsigned)t.rgba[0],
                       (unsigned)t.rgba[1],
                       (unsigned)t.rgba[2],
                       (unsigned)t.rgba[3]);
    }
    g_assert_true(t.read_ok);
}

/* ── visual mode: janela maior, mesma limpeza; você confirma olhando ───────── */

static gboolean
on_render_visual(GtkGLArea* area, GdkGLContext* context, gpointer user_data)
{
    (void)user_data;
    (void)context;

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area))
        return TRUE;

    gtk_gl_area_attach_buffers(area);

    int w = gtk_widget_get_width(GTK_WIDGET(area));
    int h = gtk_widget_get_height(GTK_WIDGET(area));
    if (w < 4 || h < 4) {
        gtk_gl_area_queue_render(area);
        return TRUE;
    }

    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.85f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    unsigned char px[4];
    glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    g_print("[visual] glReadPixels(centro): R=%u G=%u B=%u A=%u\n",
            (unsigned)px[0],
            (unsigned)px[1],
            (unsigned)px[2],
            (unsigned)px[3]);

    return TRUE;
}

static void
activate_visual(GtkApplication* app, gpointer user_data)
{
    (void)user_data;
    GtkWidget* win = gtk_application_window_new(app);
    GtkWidget* gl  = gtk_gl_area_new();

    gtk_window_set_title(GTK_WINDOW(win),
                         "milkdrop test — área verde = FBO GtkGLArea OK (feche a janela)");
    gtk_gl_area_set_required_version(GTK_GL_AREA(gl), 3, 3);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(gl), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 360);
    gtk_window_set_child(GTK_WINDOW(win), gl);
    g_signal_connect(gl, "render", G_CALLBACK(on_render_visual), NULL);

    g_print("Modo visual: se o GLArea estiver VERDE, cópia/leitura do framebuffer interno está coerente.\n");
    gtk_window_present(GTK_WINDOW(win));
    gtk_gl_area_queue_render(GTK_GL_AREA(gl));
}

static int
run_visual(char* progname)
{
    char* argv[] = {progname, NULL};

    GtkApplication* app = gtk_application_new(
        "io.github.mauriciobc.milkdrop.TestGlareaFboVisual",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate_visual), NULL);
    int st = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return st;
}

int
main(int argc, char** argv)
{
    /* GApplication rejeita --visual; tratamos antes do run. */
    if (argc >= 2 && strcmp(argv[1], "--visual") == 0)
        return run_visual(argv[0]);

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/gtk-glarea-fbo/green-readback", test_glarea_fbo_green_readback);
    return g_test_run();
}
