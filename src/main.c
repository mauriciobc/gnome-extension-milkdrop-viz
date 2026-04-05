#define G_LOG_DOMAIN "milkdrop"

#include "app.h"
#include "audio.h"
#include "control.h"
#include "presets.h"
#include "quarantine.h"
#include "renderer.h"

#include <string.h>
#include <dlfcn.h>

#if HAVE_PROJECTM

#include <epoxy/gl.h>
#include <EGL/egl.h>
#include <projectM-4/core.h>
#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>
#include <projectM-4/audio.h>
#include <projectM-4/playlist_playback.h>
#include <projectM-4/playlist_callbacks.h>
#include <projectM-4/playlist_core.h>
#include <projectM-4/playlist_items.h>
#include <projectM-4/playlist_memory.h>
#include <projectM-4/playlist_types.h>

static gboolean milkdrop_verbose_gl          = FALSE;
static gboolean milkdrop_logged_render_fbo   = FALSE;
static gint64   milkdrop_pm_mono_origin_us   = 0;

static void
milkdrop_configure_texture_search_paths(AppData* app_data)
{
    const char* texture_paths[1] = {0};
    size_t count = 0;

    if (!app_data || !app_data->projectm)
        return;

    if (app_data->preset_dir && app_data->preset_dir[0] != '\0' &&
        g_file_test(app_data->preset_dir, G_FILE_TEST_IS_DIR)) {
        texture_paths[0] = app_data->preset_dir;
        count = 1;
    }

    projectm_set_texture_search_paths(app_data->projectm,
                                      count > 0 ? texture_paths : NULL,
                                      count);
}

static void
milkdrop_on_preset_switch_failed(const char* preset_filename,
                                 const char* message,
                                 void*       user_data)
{
    AppData* app_data = user_data;

    if (!app_data)
        return;

    g_warning("Preset switch failed for %s: %s",
              preset_filename ? preset_filename : "(unknown)",
              message ? message : "(no message)");

    quarantine_record_failure(app_data, preset_filename);

    if (atomic_load(&app_data->quarantine_all_failed))
        g_warning("milkdrop: %d consecutive preset failures — stopping auto-advance",
                  app_data->consecutive_failures);
}

static void
milkdrop_on_preset_switched(bool         is_hard_cut,
                             unsigned int index,
                             void*        user_data)
{
    (void)is_hard_cut;
    AppData* app_data = user_data;

    if (!app_data || !app_data->projectm_playlist)
        return;

    char* filename = projectm_playlist_item(app_data->projectm_playlist, index);
    quarantine_record_success(app_data, filename);
    if (filename)
        projectm_playlist_free_string(filename);
}

static void
milkdrop_sync_playlist_from_preset_dir(AppData* app_data)
{
    if (!app_data || !app_data->projectm || !app_data->projectm_playlist)
        return;

    milkdrop_configure_texture_search_paths(app_data);

    projectm_playlist_clear(app_data->projectm_playlist);

    uint32_t added = 0;
    if (app_data->preset_dir && app_data->preset_dir[0] != '\0')
        added = projectm_playlist_add_path(app_data->projectm_playlist,
                                           app_data->preset_dir,
                                           true,
                                           false);

    projectm_playlist_set_shuffle(app_data->projectm_playlist,
                                  atomic_load(&app_data->shuffle_runtime));

    if (added > 0) {
        projectm_playlist_set_position(app_data->projectm_playlist, 0u, true);
        if (app_data->verbose)
            g_message("playlist loaded %u presets from %s", added, app_data->preset_dir);
    } else {
        projectm_load_preset_file(app_data->projectm, "idle://", false);
        if (app_data->verbose)
            g_message("playlist empty for %s, using idle preset",
                      app_data->preset_dir ? app_data->preset_dir : "(none)");
    }
}

G_GNUC_UNUSED static void*
gl_load_proc(const char* name, void* user_data)
{
    (void)user_data;
    void* func = (void*)eglGetProcAddress(name);
    if (!func)
        func = dlsym(RTLD_DEFAULT, name);
    if (milkdrop_verbose_gl)
        g_message("gl_load_proc: %s -> %p", name, func);
    else if (!func)
        g_warning("gl_load_proc: %s resolved NULL", name);
    return func;
}

/**
 * Inicializa libprojectM na primeira vez que o contexto GL estiver utilizável.
 * on_realize pode correr antes de GdkGLArea expor um contexto — nesse caso
 * projectm ficava NULL para sempre e renderer_frame_prep nunca desenhava.
 */
static gboolean
milkdrop_try_init_projectm(GtkGLArea* area, AppData* app_data)
{
    if (!area || !app_data || app_data->projectm)
        return TRUE;

    if (atomic_load(&app_data->projectm_init_aborted))
        return FALSE;

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area))
        return FALSE;

    GdkGLContext* gl_ctx = gtk_gl_area_get_context(area);
    if (!gl_ctx)
        return FALSE;

    int major = 0, minor = 0;
    gdk_gl_context_get_version(gl_ctx, &major, &minor);
    if (app_data->verbose) {
        g_message("GLArea context: OpenGL %d.%d", major, minor);
        g_message("Forward compat: %d", gdk_gl_context_get_forward_compatible(gl_ctx));
        g_message("Debug enabled: %d", gdk_gl_context_get_debug_enabled(gl_ctx));
    }

    if (major < 3 || (major == 3 && minor < 3)) {
        static gboolean logged_version;
        if (!logged_version) {
            g_warning("OpenGL %d.%d is below the required 3.3 — projectM disabled",
                      major, minor);
            logged_version = TRUE;
        }
        atomic_store(&app_data->projectm_init_aborted, true);
        return FALSE;
    }

    if (app_data->verbose)
        g_message("Calling projectm_create()...");
    app_data->projectm = projectm_create();
    if (!app_data->projectm) {
        static gboolean logged_create;
        if (!logged_create) {
            g_warning("libprojectM failed to initialize (projectm_create returned NULL)");
            logged_create = TRUE;
        }
        atomic_store(&app_data->projectm_init_aborted, true);
        return FALSE;
    }
    if (app_data->verbose)
        g_message("projectm_create() succeeded");

    app_data->projectm_playlist = projectm_playlist_create(app_data->projectm);
    if (!app_data->projectm_playlist) {
        g_warning("libprojectM playlist manager failed to initialize");
        projectm_destroy(app_data->projectm);
        app_data->projectm = NULL;
        atomic_store(&app_data->projectm_init_aborted, true);
        return FALSE;
    }
    if (app_data->verbose)
        g_message("projectm_playlist_create() succeeded");

    projectm_playlist_set_preset_switch_failed_event_callback(app_data->projectm_playlist,
                                                              milkdrop_on_preset_switch_failed,
                                                              app_data);
    projectm_playlist_set_preset_switched_event_callback(app_data->projectm_playlist,
                                                         milkdrop_on_preset_switched,
                                                         app_data);

    if (app_data->render_width > 0 && app_data->render_height > 0)
        projectm_set_window_size(app_data->projectm,
                                 (size_t)app_data->render_width,
                                 (size_t)app_data->render_height);

    milkdrop_sync_playlist_from_preset_dir(app_data);

    return TRUE;
}
#endif

/* ~60 Hz via GLib: não usar só gtk_widget_add_tick_callback — com a janela ancorada
 * por baixo (wallpaper), o GdkFrameClock do GTK pode não sinalizar frames e o
 * projectM fica estático até um resize “acordar” o relógio. */
#define MILKDROP_RENDER_PULSE_INTERVAL_MS 16

/* Após o render GL, pedir frame na superfície no próximo idle — dentro do próprio
 * callback render o GDK ainda pode estar a fechar o frame; no Wayland isto ajuda
 * o compositor a voltar a pedir buffers (clone/wallpaper atualiza). */
static gboolean
milkdrop_idle_surface_queue_render(gpointer user_data)
{
    AppData* app_data = user_data;
    if (app_data->window) {
        GtkNative* native = gtk_widget_get_native(GTK_WIDGET(app_data->window));
        if (native) {
            GdkSurface* surface = gtk_native_get_surface(native);
            if (surface)
                gdk_surface_queue_render(surface);
        }
    }
    atomic_store(&app_data->idle_surface_queued, false);
    return G_SOURCE_REMOVE;
}

static gboolean
on_render_pulse(gpointer user_data)
{
    AppData* app_data = user_data;

#if !HAVE_PROJECTM
    if (atomic_exchange(&app_data->preset_dir_pending, false)) {
        char next_dir[sizeof(app_data->pending_preset_dir)] = {0};
        g_mutex_lock(&app_data->preset_dir_lock);
        g_strlcpy(next_dir, app_data->pending_preset_dir, sizeof(next_dir));
        g_mutex_unlock(&app_data->preset_dir_lock);

        g_clear_pointer(&app_data->preset_dir, g_free);
        app_data->preset_dir = g_strdup(next_dir);
        if (!presets_reload(app_data))
            g_warning("Failed to reload presets from %s", next_dir);
    }
#endif

    if (app_data->window) {
        static float last_opacity = -1.0f;
        float current_opacity = atomic_load(&app_data->opacity);
        if (current_opacity != last_opacity) {
            gtk_widget_set_opacity(GTK_WIDGET(app_data->window), current_opacity);
            last_opacity = current_opacity;
        }
    }

    if (app_data->verbose) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count % 120 == 0)
            g_message("render-pulse: frame_count=%d, projectm=%p, render_size=%dx%d",
                      frame_count, app_data->projectm, app_data->render_width, app_data->render_height);
    }

    /* Não exigir mapped: janela ancorada como wallpaper pode ficar num estado em que
     * get_mapped é falso e nunca pedíamos frame — imagem estática até resize. */
    if (app_data->gl_area)
        gtk_gl_area_queue_render(app_data->gl_area);

    return G_SOURCE_CONTINUE;
}

static void
make_surface_click_through(GtkWidget* widget)
{
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (!surface)
        return;

    cairo_region_t* region = cairo_region_create();
    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);
}

/* Called each time the renderer window is mapped (i.e. shown after realize).
 * Re-applying the empty input region here ensures the Wayland compositor
 * commits the click-through behaviour, even if the surface was not ready
 * when make_surface_click_through() was first called after gtk_window_present().
 */
static void
on_window_map(GtkWidget* widget, gpointer user_data)
{
    (void)user_data;
    make_surface_click_through(widget);
}

static void
on_realize(GtkGLArea* area, gpointer user_data)
{
    AppData* app_data = user_data;

#if HAVE_PROJECTM
    atomic_store(&app_data->projectm_init_aborted, false);
#endif

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) {
        g_warning("GtkGLArea failed to initialize a GL context");
        return;
    }

#if HAVE_PROJECTM
    if (!milkdrop_try_init_projectm(area, app_data) && app_data->verbose)
        g_message("projectM init deferred until GL context is ready (next render)");
#else
    if (app_data->verbose)
        g_message("Renderer running in placeholder mode (libprojectM not available)");
#endif
}

static void
on_unrealize(GtkGLArea* area, gpointer user_data)
{
    (void)area;
    AppData* app_data = user_data;

#if HAVE_PROJECTM
    if (app_data->projectm) {
        if (app_data->projectm_playlist) {
            projectm_playlist_destroy(app_data->projectm_playlist);
            app_data->projectm_playlist = NULL;
        }
        projectm_destroy(app_data->projectm);
        app_data->projectm = NULL;
    }
    milkdrop_pm_mono_origin_us = 0;
#endif

#if !HAVE_PROJECTM
    (void)app_data;
#endif
}

static void
on_resize(GtkGLArea* area, int width, int height, gpointer user_data)
{
    AppData* app_data = user_data;
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    if (scale < 1)
        scale = 1;
    /* GtkGLArea::resize passes backing-store dimensions (logical size × scale), not
     * gtk_widget_get_width/height. renderer_apply_resize() expects logical × scale. */
    renderer_apply_resize(app_data, width / scale, height / scale, scale);
}

static gboolean
on_render(GtkGLArea* area, GdkGLContext* context, gpointer user_data)
{
    (void)context;
    AppData* app_data = user_data;

#if HAVE_PROJECTM
    if (!app_data->projectm && !milkdrop_try_init_projectm(area, app_data))
        return TRUE;

    if (atomic_exchange(&app_data->preset_dir_pending, false)) {
        char next_dir[sizeof(app_data->pending_preset_dir)] = {0};

        g_mutex_lock(&app_data->preset_dir_lock);
        g_strlcpy(next_dir, app_data->pending_preset_dir, sizeof(next_dir));
        g_mutex_unlock(&app_data->preset_dir_lock);

        g_clear_pointer(&app_data->preset_dir, g_free);
        app_data->preset_dir = g_strdup(next_dir);
        milkdrop_sync_playlist_from_preset_dir(app_data);
    }

    if (app_data->projectm && app_data->projectm_playlist) {
        bool do_next = atomic_exchange(&app_data->next_preset_pending, false);
        bool do_prev = atomic_exchange(&app_data->prev_preset_pending, false);

        /* projectM preset loading must happen with a current GL context. */
        if (do_next) {
            projectm_playlist_play_next(app_data->projectm_playlist, true);
        } else if (do_prev) {
            projectm_playlist_play_previous(app_data->projectm_playlist, true);
        }
    }

    /* F2: skip rendering until on_resize() has supplied real dimensions.  GTK
     * guarantees on_resize fires before the first on_render, so render_width
     * should always be non-zero in steady state; the guard protects against
     * any edge case where the surface has not been sized yet. */
    {
        RendererFramePrep prep;
        float               pcm_samples[MILKDROP_RING_CAPACITY];

        renderer_frame_prep(app_data, pcm_samples, G_N_ELEMENTS(pcm_samples), &prep);
        if (prep.would_draw) {
            /* GtkGLArea makes the GL context current before emitting "render". */

            /* GtkGLArea draws to an internal FBO, not GL framebuffer 0. projectM's
             * projectm_opengl_render_frame() targets FBO 0 — wrong buffer → blank/white window. */
            gtk_gl_area_attach_buffers(area);

            if (milkdrop_pm_mono_origin_us == 0)
                milkdrop_pm_mono_origin_us = g_get_monotonic_time();
            projectm_set_frame_time(
                app_data->projectm,
                (g_get_monotonic_time() - milkdrop_pm_mono_origin_us) / 1000000.0);

            if (prep.would_feed_pcm)
                projectm_pcm_add_float(app_data->projectm,
                                       pcm_samples,
                                       prep.stereo_frames,
                                       PROJECTM_STEREO);

            {
                static bool last_shuffle = false;
                bool current_shuffle = atomic_load(&app_data->shuffle_runtime);
                if (current_shuffle != last_shuffle) {
                    projectm_playlist_set_shuffle(app_data->projectm_playlist,
                                                  current_shuffle);
                    last_shuffle = current_shuffle;
                }
            }

            GLint draw_fbo = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
            if (draw_fbo != 0) {
                if (app_data->verbose && !milkdrop_logged_render_fbo) {
                    g_message("projectM render: using GtkGLArea FBO %d (not framebuffer 0)",
                              (int)draw_fbo);
                    milkdrop_logged_render_fbo = TRUE;
                }
                projectm_opengl_render_frame_fbo(app_data->projectm, (uint32_t)draw_fbo);

                /* Verificar erros GL: alguns presets com GetBlur causam
                 * GL_INVALID_FRAMEBUFFER_OPERATION (0x506) ao renderizar em FBO externo.
                 * Aprendizado do teste: fallback para render_frame() quando isso ocorre. */
                GLenum err = glGetError();
                if (err == GL_INVALID_FRAMEBUFFER_OPERATION) {
                    if (app_data->verbose)
                        g_message("projectM FBO error 0x%x — frame dropped (GetBlur preset?)", err);
                    /* Do NOT fall back to projectm_opengl_render_frame(): it targets FBO 0,
                     * which is wrong inside GtkGLArea and produces blank/white output.
                     * Dropping the frame leaves the last good frame visible. */
                } else if (err != GL_NO_ERROR && app_data->verbose) {
                    g_message("projectM GL error: 0x%x", err);
                }
            } else {
                projectm_opengl_render_frame(app_data->projectm);
            }

            /* Estado que o GTK valida após o sinal render; projectM pode alterar. */
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);

            if (app_data->window && !atomic_exchange(&app_data->idle_surface_queued, true))
                g_idle_add(milkdrop_idle_surface_queue_render, app_data);
        }
    }
#endif

#if !HAVE_PROJECTM
    (void)app_data;
#endif

    return TRUE;
}

static gboolean
on_close_request(GtkWidget* widget, gpointer user_data)
{
    (void)widget;
    (void)user_data;
    return GDK_EVENT_STOP;
}

static void
build_window(AppData* app_data)
{
    GtkWidget* window = gtk_application_window_new(app_data->app);
    GtkWidget* gl_area = gtk_gl_area_new();

    app_data->window = GTK_WINDOW(window);
    app_data->gl_area = GTK_GL_AREA(gl_area);

    gtk_window_set_title(app_data->window, "milkdrop");

    // Avoid fullscreen/maximized state because dock/intellihide extensions may
    // treat the renderer as a normal fullscreen app and hide themselves.
    int default_width = 1280;
    int default_height = 720;
    GdkDisplay* display = gtk_widget_get_display(window);
    if (display) {
        GListModel* monitors = gdk_display_get_monitors(display);
        guint n = g_list_model_get_n_items(monitors);
        guint idx = 0;
        if (n > 0) {
            if (app_data->monitor_index >= 0 && (guint)app_data->monitor_index < n)
                idx = (guint)app_data->monitor_index;

            gpointer item = g_list_model_get_item(monitors, idx);
            if (item) {
                GdkMonitor* monitor = GDK_MONITOR(item);
                GdkRectangle geometry = {0};
                gdk_monitor_get_geometry(monitor, &geometry);
                if (geometry.width > 0 && geometry.height > 0) {
                    default_width = geometry.width;
                    default_height = geometry.height;
                } else {
                    g_warning("Monitor %u returned invalid geometry %dx%d — using default %dx%d",
                              idx, geometry.width, geometry.height, default_width, default_height);
                }
                g_object_unref(monitor);
            }
        }
    }

    gtk_window_set_default_size(app_data->window, default_width, default_height);
    gtk_window_set_decorated(app_data->window, false);
    gtk_window_set_resizable(app_data->window, false);

    // Keep renderer strictly non-interactive so desktop/input is never blocked.
    gtk_widget_set_focusable(GTK_WIDGET(app_data->window), FALSE);
    gtk_widget_set_can_target(gl_area, FALSE);
    gtk_widget_set_focusable(gl_area, FALSE);

    gtk_gl_area_set_auto_render(app_data->gl_area, false);
    gtk_gl_area_set_has_depth_buffer(app_data->gl_area, false);
    gtk_gl_area_set_has_stencil_buffer(app_data->gl_area, false);

    /* projectM 4.x requires OpenGL 3.3 core profile */
    gtk_gl_area_set_required_version(GTK_GL_AREA(gl_area), 3, 3);
#if GTK_CHECK_VERSION(4, 12, 0)
    /* projectM precisa de OpenGL desktop; se o fundo ficar preto, experimente
     * MILKDROP_FORCE_GL_API=1 no ambiente do renderer. */
    if (g_getenv("MILKDROP_FORCE_GL_API"))
        gtk_gl_area_set_allowed_apis(GTK_GL_AREA(gl_area), GDK_GL_API_GL);
#endif

    g_signal_connect(app_data->gl_area, "realize", G_CALLBACK(on_realize), app_data);
    g_signal_connect(app_data->gl_area, "unrealize", G_CALLBACK(on_unrealize), app_data);
    g_signal_connect(app_data->gl_area, "resize", G_CALLBACK(on_resize), app_data);
    g_signal_connect(app_data->gl_area, "render", G_CALLBACK(on_render), app_data);

    gtk_window_set_child(app_data->window, gl_area);

    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), app_data);

    /* Re-apply click-through on every map event so the input region is
     * guaranteed to be committed by the Wayland compositor. */
    g_signal_connect_after(window, "map", G_CALLBACK(on_window_map), NULL);

    app_data->render_pulse_source_id =
        g_timeout_add(MILKDROP_RENDER_PULSE_INTERVAL_MS, on_render_pulse, app_data);
    gtk_window_present(app_data->window);
    gtk_widget_set_opacity(GTK_WIDGET(app_data->window), atomic_load(&app_data->opacity));

    /* Belt-and-suspenders: also call immediately after present() in case the
     * surface was already mapped synchronously (e.g. on X11). */
    make_surface_click_through(GTK_WIDGET(app_data->window));

    g_message("renderer: geometry=%dx%d on monitor=%d, click-through=enabled",
              default_width, default_height, app_data->monitor_index);
}

static void
on_activate(GApplication* application, gpointer user_data)
{
    AppData* app_data = user_data;

    if (app_data->window) {
        gtk_window_present(app_data->window);
        return;
    }

    app_data->app = GTK_APPLICATION(application);

    if (app_data->preset_dir && !presets_reload(app_data))
        g_warning("Failed to scan presets in %s", app_data->preset_dir);

    build_window(app_data);

    if (!audio_init(app_data))
        g_warning("Audio backend failed to initialize");

    if (!control_init(app_data))
        g_warning("Control backend failed to initialize");
}

static void
on_shutdown(GApplication* application, gpointer user_data)
{
    (void)application;
    AppData* app_data = user_data;

    if (app_data->render_pulse_source_id > 0) {
        g_source_remove(app_data->render_pulse_source_id);
        app_data->render_pulse_source_id = 0;
    }

    control_cleanup(app_data);
    audio_cleanup(app_data);

    g_clear_pointer(&app_data->preset_dir, g_free);
    presets_clear(app_data);
    g_clear_pointer(&app_data->socket_path, g_free);
    g_mutex_clear(&app_data->preset_dir_lock);
}

int
main(int argc, char** argv)
{
    AppData app_data = {0};
    int monitor_index = 0;
    double opacity = 1.0;
    gboolean shuffle = FALSE;
    gboolean overlay = FALSE;
    gboolean verbose = FALSE;
    char* preset_dir = NULL;
    char* socket_path = NULL;

    GOptionEntry entries[] = {
        {"monitor", 0, 0, G_OPTION_ARG_INT, &monitor_index, "Monitor index", "INDEX"},
        {"opacity", 0, 0, G_OPTION_ARG_DOUBLE, &opacity, "Window opacity (0..1)", "VALUE"},
        {"preset-dir", 0, 0, G_OPTION_ARG_FILENAME, &preset_dir, "Preset directory", "PATH"},
        {"socket-path", 0, 0, G_OPTION_ARG_FILENAME, &socket_path, "Control socket path", "PATH"},
        {"shuffle", 0, 0, G_OPTION_ARG_NONE, &shuffle, "Enable shuffle mode", NULL},
        {"overlay", 0, 0, G_OPTION_ARG_NONE, &overlay, "Enable overlay mode (state + control socket)", NULL},
        {"verbose", 0, 0, G_OPTION_ARG_NONE, &verbose, "Verbose logging", NULL},
        {NULL},
    };

    GOptionContext* context = g_option_context_new("- milkdrop renderer");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, NULL)) {
        g_printerr("Failed to parse command line arguments\n");
        g_option_context_free(context);
        return 1;
    }
    g_option_context_free(context);

    g_set_prgname("milkdrop");

    app_data.verbose = verbose;
#if HAVE_PROJECTM
    milkdrop_verbose_gl = verbose;
    atomic_store(&app_data.projectm_init_aborted, false);
#endif

    app_data.monitor_index = monitor_index;
    app_data.preset_dir = preset_dir;
    app_data.presets = NULL;
    app_data.preset_count = 0;
    app_data.preset_current = 0;
    app_data.socket_path = socket_path;
    app_data.shuffle = (bool)shuffle;
    atomic_store(&app_data.opacity, (float)CLAMP(opacity, 0.0, 1.0));
    atomic_store(&app_data.pause_audio, false);
    atomic_store(&app_data.shutdown_requested, false);
    atomic_store(&app_data.overlay_enabled, overlay);
    atomic_store(&app_data.shuffle_runtime, app_data.shuffle);
    atomic_store(&app_data.preset_dir_pending, false);
    atomic_store(&app_data.next_preset_pending, false);
    atomic_store(&app_data.prev_preset_pending, false);
    atomic_store(&app_data.control_thread_running, false);
    app_data.control_fd = -1;
    app_data.control_thread = NULL;
    g_mutex_init(&app_data.preset_dir_lock);
    app_data.pending_preset_dir[0] = '\0';

    /* No explicit app-id: GDK Wayland will use g_get_prgname() ("milkdrop") as
     * the xdg_toplevel app_id.  This ensures Meta.Window.get_wm_class() in the
     * extension returns "milkdrop", matching the ownership check in extension.js. */
    GtkApplication* app = gtk_application_new(
      NULL,
      G_APPLICATION_NON_UNIQUE
    );

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &app_data);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &app_data);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}