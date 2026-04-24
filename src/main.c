#define G_LOG_DOMAIN "milkdrop"

#include "app.h"
#include "audio.h"
#include "control.h"
#include "offscreen_renderer.h"
#include "quarantine.h"
#include "renderer.h"

#include <stdio.h>
#include <string.h>

#if HAVE_PROJECTM

#include <projectM-4/core.h>
#include <projectM-4/callbacks.h>
#include <projectM-4/logging.h>
#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>
#include <projectM-4/audio.h>
#include <epoxy/gl.h>

static gboolean
milkdrop_str_contains_ci(const char* haystack,
                         const char* needle)
{
    if (!haystack || !needle)
        return FALSE;

    size_t nlen = strlen(needle);
    if (nlen == 0)
        return FALSE;

    for (const char* p = haystack; *p; p++) {
        if (g_ascii_strncasecmp(p, needle, nlen) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
milkdrop_projectm_message_is_texture_failure(const char* msg)
{
    static const char* const failure_needles[] = {
        "failed to find",
        "failed to load",
        "failed to create",
        "could not load",
        "could not open",
        "unable to load",
        "unable to open",
        "cannot open",
        "file not found",
        "no such file",
        "missing image",
        "missing texture",
    };

    if (!msg)
        return FALSE;

    for (size_t i = 0; i < G_N_ELEMENTS(failure_needles); i++) {
        if (milkdrop_str_contains_ci(msg, failure_needles[i]))
            return TRUE;
    }
    return FALSE;
}

static void
milkdrop_projectm_log(const char*           message,
                        projectm_log_level    level,
                        void*                 user_data)
{
    AppData* app_data = user_data;
    gboolean verbose  = app_data && app_data->verbose;

    if (!message)
        return;

    if (level == PROJECTM_LOG_LEVEL_FATAL)
        g_critical("milkdrop: projectM: %s", message);
    else if (level >= PROJECTM_LOG_LEVEL_ERROR)
        g_warning("milkdrop: projectM: %s", message);
    else if (level >= PROJECTM_LOG_LEVEL_WARN)
        g_warning("milkdrop: projectM: %s", message);
    else if (level >= PROJECTM_LOG_LEVEL_INFO &&
             milkdrop_projectm_message_is_texture_failure(message))
        g_warning("milkdrop: missing texture: %s", message);
    else if (verbose)
        g_message("milkdrop: projectM: %s", message);
}

static void
milkdrop_register_projectm_logging(AppData* app_data)
{
    projectm_set_log_callback(milkdrop_projectm_log, true, app_data);
    projectm_set_log_level(PROJECTM_LOG_LEVEL_INFO, true);
}

static void
milkdrop_unregister_projectm_logging(void)
{
    projectm_set_log_callback(NULL, true, NULL);
}

/* Load an atomic setting, call the setter only when the value has changed,
 * and cache the last-synced value.  Keeps the render loop free of per-frame
 * API calls when nothing changed. */
#define SYNC_ATOMIC(field, synced_field, setter_expr)              \
    do {                                                           \
        __typeof__(atomic_load(&app_data->field)) _cur =           \
            atomic_load(&app_data->field);                         \
        if (_cur != app_data->synced_field) {                      \
            setter_expr;                                           \
            app_data->synced_field = _cur;                         \
        }                                                          \
    } while (0)

static void
milkdrop_configure_texture_search_paths(AppData* app_data)
{
    const char* texture_paths[2] = {0};
    size_t count = 0;
    char        preset_dir_buf[MILKDROP_PATH_MAX];

    if (!app_data || !app_data->projectm)
        return;

    preset_dir_buf[0] = '\0';
    g_mutex_lock(&app_data->preset_dir_lock);
    if (app_data->preset_dir)
        g_strlcpy(preset_dir_buf, app_data->preset_dir, sizeof(preset_dir_buf));
    g_mutex_unlock(&app_data->preset_dir_lock);

    if (preset_dir_buf[0] != '\0' && g_file_test(preset_dir_buf, G_FILE_TEST_IS_DIR)) {
        texture_paths[count++] = preset_dir_buf;

        g_clear_pointer(&app_data->textures_dir, g_free);
        app_data->textures_dir = g_build_filename(preset_dir_buf, "textures", NULL);
        if (app_data->textures_dir && g_file_test(app_data->textures_dir, G_FILE_TEST_IS_DIR)) {
            texture_paths[count++] = app_data->textures_dir;
        } else {
            g_clear_pointer(&app_data->textures_dir, g_free);
        }
    }

    projectm_set_texture_search_paths(app_data->projectm,
                                      count > 0 ? texture_paths : NULL,
                                      count);
}

static void
milkdrop_on_playlist_preset_switched(bool         is_hard_cut,
                                     unsigned int index,
                                     void*        user_data)
{
    (void)is_hard_cut;
    AppData* app_data = user_data;
    if (!app_data || !app_data->playlist)
        return;

    char* filename = projectm_playlist_item(app_data->playlist, index);
    if (filename) {
        g_mutex_lock(&app_data->quarantine.last_preset_lock);
        g_strlcpy(app_data->quarantine.last_good_preset, filename, sizeof(app_data->quarantine.last_good_preset));
        g_mutex_unlock(&app_data->quarantine.last_preset_lock);
        quarantine_record_success(app_data, filename);
        if (app_data->verbose)
            g_message("preset_switched: index=%u, filename=%s", index, filename);
        projectm_playlist_free_string(filename);
    }
}

static void
milkdrop_on_playlist_preset_switch_failed(const char* preset_filename,
                                          const char* message,
                                          void*       user_data)
{
    AppData* app_data = user_data;

    if (!app_data)
        return;

    if (g_strcmp0(preset_filename, "idle://") == 0)
        return;

    g_warning("Preset switch failed for %s: %s",
              preset_filename ? preset_filename : "(unknown)",
              message ? message : "(no message)");

    quarantine_record_failure(app_data, preset_filename);

    if (atomic_load(&app_data->quarantine.all_failed))
        g_warning("milkdrop: %d consecutive preset failures — playlist will retry",
                  app_data->quarantine.consecutive_failures);
}

static void
milkdrop_sync_playlist_from_preset_dir(AppData* app_data)
{
    char dir_snapshot[MILKDROP_PATH_MAX];
    uint32_t count = 0;

    if (!app_data || !app_data->projectm || !app_data->playlist)
        return;

    dir_snapshot[0] = '\0';
    g_mutex_lock(&app_data->preset_dir_lock);
    if (app_data->preset_dir)
        g_strlcpy(dir_snapshot, app_data->preset_dir, sizeof(dir_snapshot));
    g_mutex_unlock(&app_data->preset_dir_lock);

    g_message("playlist: syncing from preset_dir=%s",
              dir_snapshot[0] != '\0' ? dir_snapshot : "(null)");

    projectm_playlist_clear(app_data->playlist);
    milkdrop_configure_texture_search_paths(app_data);

    if (dir_snapshot[0] != '\0' && g_file_test(dir_snapshot, G_FILE_TEST_IS_DIR)) {
        count = projectm_playlist_add_path(app_data->playlist, dir_snapshot, true, false);
        g_message("playlist: added %u presets from %s", count, dir_snapshot);
        if (count > 0)
            projectm_playlist_play_next(app_data->playlist, true);
    } else if (dir_snapshot[0] != '\0') {
        g_warning("playlist: preset_dir is not a directory: %s", dir_snapshot);
    }
}

static void     milkdrop_maybe_resize_picture(AppData* app_data);
static gboolean milkdrop_render_frame(AppData* app_data);

static gboolean
milkdrop_try_init_projectm(AppData* app_data)
{
    if (!app_data || app_data->projectm)
        return TRUE;

    if (atomic_load(&app_data->projectm_init_aborted))
        return FALSE;

    if (!app_data->offscreen &&
        !(app_data->offscreen = offscreen_renderer_new()))
        goto fail;

    if (!offscreen_renderer_init(app_data->offscreen,
                                 MAX(app_data->render_width, 1),
                                 MAX(app_data->render_height, 1),
                                 app_data->verbose))
        goto fail;

    if (!offscreen_renderer_make_current(app_data->offscreen))
        goto fail;

    milkdrop_register_projectm_logging(app_data);

    if (app_data->verbose)
        g_message("Calling projectm_create_with_opengl_load_proc()...");
    app_data->projectm =
        projectm_create_with_opengl_load_proc(offscreen_renderer_gl_load_proc, NULL);
    if (!app_data->projectm) {
        static gboolean logged_create;
        if (!logged_create) {
            g_warning("libprojectM failed to initialize (offscreen projectm_create returned NULL)");
            logged_create = TRUE;
        }
        goto fail;
    }
    if (app_data->verbose)
        g_message("projectm_create() succeeded");

    app_data->playlist = projectm_playlist_create(app_data->projectm);
    if (!app_data->playlist) {
        g_warning("libprojectM: failed to create playlist manager");
        goto fail;
    }

    projectm_playlist_set_preset_switched_event_callback(app_data->playlist,
                                                         milkdrop_on_playlist_preset_switched,
                                                         app_data);
    projectm_playlist_set_preset_switch_failed_event_callback(app_data->playlist,
                                                              milkdrop_on_playlist_preset_switch_failed,
                                                              app_data);

    if (app_data->render_width == 0 || app_data->render_height == 0)
        renderer_apply_resize(app_data, app_data->initial_width, app_data->initial_height, 1);

    if (app_data->render_width > 0 && app_data->render_height > 0)
        projectm_set_window_size(app_data->projectm,
                                 (size_t)app_data->render_width,
                                 (size_t)app_data->render_height);

    milkdrop_sync_playlist_from_preset_dir(app_data);

    projectm_playlist_set_shuffle(app_data->playlist,
                                   atomic_load(&app_data->shuffle_runtime));

    projectm_set_preset_duration(app_data->projectm,
                                (double)atomic_load(&app_data->preset_rotation_interval));

    int init_fps = atomic_load(&app_data->fps_runtime);
    if (init_fps <= 0)
        init_fps = 60;
    projectm_set_fps(app_data->projectm, init_fps);
    app_data->last_synced_projectm_fps = init_fps;

    projectm_set_mesh_size(app_data->projectm, 32, 24);
    projectm_set_aspect_correction(app_data->projectm, true);

    app_data->pcm_max_samples_per_channel = projectm_pcm_get_max_samples();

    projectm_set_beat_sensitivity(app_data->projectm,
                                  atomic_load(&app_data->transitions.beat_sensitivity));
    projectm_set_hard_cut_enabled(app_data->projectm,
                                  atomic_load(&app_data->transitions.hard_cut_enabled));
    projectm_set_hard_cut_sensitivity(app_data->projectm,
                                      atomic_load(&app_data->transitions.hard_cut_sensitivity));
    projectm_set_hard_cut_duration(app_data->projectm,
                                   atomic_load(&app_data->transitions.hard_cut_duration));
    projectm_set_soft_cut_duration(app_data->projectm,
                                   atomic_load(&app_data->transitions.soft_cut_duration));
    app_data->transitions.last_synced_beat_sensitivity     = atomic_load(&app_data->transitions.beat_sensitivity);
    app_data->transitions.last_synced_hard_cut_enabled     = atomic_load(&app_data->transitions.hard_cut_enabled);
    app_data->transitions.last_synced_hard_cut_sensitivity = atomic_load(&app_data->transitions.hard_cut_sensitivity);
    app_data->transitions.last_synced_hard_cut_duration    = atomic_load(&app_data->transitions.hard_cut_duration);
    app_data->transitions.last_synced_soft_cut_duration    = atomic_load(&app_data->transitions.soft_cut_duration);

    return TRUE;

fail:
    /* offscreen_renderer_shutdown is null-safe and zero-init-safe:
     * it skips SDL_GL_MakeCurrent if gl_context is NULL, skips glDelete*
     * if fbo/tex are 0, and only calls SDL_DestroyWindow if window exists.
     * Calling it on a partially-initialized renderer is safe. */
    if (app_data->playlist) {
        projectm_playlist_destroy(app_data->playlist);
        app_data->playlist = NULL;
    }
    if (app_data->projectm) {
        projectm_destroy(app_data->projectm);
        app_data->projectm = NULL;
    }
    if (app_data->offscreen) {
        offscreen_renderer_shutdown(app_data->offscreen);
    }
    atomic_store(&app_data->projectm_init_aborted, true);
    return FALSE;
}
#endif

/* ~60 Hz via GLib: não usar só gtk_widget_add_tick_callback — com a janela ancorada
 * por baixo (wallpaper), o GdkFrameClock do GTK pode não sinalizar frames e o
 * projectM fica estático até um resize "acordar" o relógio. */
#define MILKDROP_RENDER_PULSE_INTERVAL_MS 16

static gboolean
on_render_pulse(gpointer user_data)
{
    AppData* app_data = user_data;

    {
        int target_fps = atomic_load(&app_data->fps_runtime);
        if (target_fps <= 0)
            target_fps = 60;
        if (target_fps != app_data->fps_applied) {
            app_data->fps_applied = target_fps;
            guint interval_ms = (guint)(1000u / (guint)target_fps);
            if (interval_ms < 1)
                interval_ms = 1;
            app_data->render_pulse_source_id =
                g_timeout_add(interval_ms, on_render_pulse, app_data);
            return G_SOURCE_REMOVE;
        }
    }

    if (app_data->window) {
        float current_opacity = atomic_load(&app_data->opacity);
        if (current_opacity != app_data->last_synced_opacity) {
            gtk_widget_set_opacity(GTK_WIDGET(app_data->window), current_opacity);
            app_data->last_synced_opacity = current_opacity;
        }
    }

    milkdrop_maybe_resize_picture(app_data);

    if (app_data->verbose) {
        if (app_data->pulse_frame_count % 120 == 0)
            g_message("render-pulse: frame_count=%d, projectm=%p, render_size=%dx%d",
                      app_data->pulse_frame_count, app_data->projectm, app_data->render_width, app_data->render_height);
        app_data->pulse_frame_count++;
    }

    (void)milkdrop_render_frame(app_data);

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
milkdrop_maybe_resize_picture(AppData* app_data)
{
    int width = 0;
    int height = 0;
    int scale = 1;

    if (!app_data || !app_data->picture)
        return;

    width = gtk_widget_get_width(GTK_WIDGET(app_data->picture));
    height = gtk_widget_get_height(GTK_WIDGET(app_data->picture));
    if (width <= 0 || height <= 0) {
        width = app_data->initial_width;
        height = app_data->initial_height;
    }

    scale = gtk_widget_get_scale_factor(GTK_WIDGET(app_data->picture));
    if (scale < 1)
        scale = 1;

    if (width > 0 && height > 0 &&
        (app_data->render_width != width * scale ||
         app_data->render_height != height * scale)) {
        renderer_apply_resize(app_data, width, height, scale);
    }
}

static void
milkdrop_save_screenshot_rgba(const char*   output_path,
                              const guint8* pixels,
                              int           width,
                              int           height,
                              gsize         stride)
{
    FILE* f = NULL;
    guint8* row_rgb = NULL;

    if (!output_path || !pixels || width <= 0 || height <= 0)
        return;

    f = fopen(output_path, "wb");
    if (!f) {
        g_warning("screenshot: falha ao abrir %s", output_path);
        return;
    }

    row_rgb = g_malloc((gsize)width * 3u);
    if (!row_rgb) {
        fclose(f);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        const guint8* row_rgba = pixels + (gsize)y * stride;
        guint8* dst = row_rgb;
        for (int x = 0; x < width; x++) {
            dst[0] = row_rgba[x * 4u + 0];
            dst[1] = row_rgba[x * 4u + 1];
            dst[2] = row_rgba[x * 4u + 2];
            dst += 3;
        }
        fwrite(row_rgb, 1, (gsize)width * 3u, f);
    }

    g_free(row_rgb);
    fclose(f);
}

static void
milkdrop_publish_frame_to_picture(AppData* app_data,
                                  const guint8* pixels,
                                  gsize          len,
                                  gsize          stride)
{
    GBytes* bytes = NULL;
    GdkTexture* texture = NULL;

    if (!app_data || !app_data->picture || !pixels || len == 0)
        return;

    /* Copy pixels: the readback buffer is reused by OffscreenRenderer.
     * g_bytes_new() copies, g_bytes_new_take() would steal the buffer. */
    bytes = g_bytes_new(pixels, len);
    texture = gdk_memory_texture_new(app_data->render_width,
                                     app_data->render_height,
                                     GDK_MEMORY_R8G8B8A8,
                                     bytes,
                                     stride);
    gtk_picture_set_paintable(app_data->picture, GDK_PAINTABLE(texture));
    g_object_unref(texture);
    g_bytes_unref(bytes);
}

static gboolean
milkdrop_render_frame(AppData* app_data)
{
#if HAVE_PROJECTM
    RendererFramePrep prep;
    guint8* pixels = NULL;
    gsize pixels_len = 0;
    gsize stride = 0;
    uint32_t target_fbo = 0;

    if (!app_data)
        return TRUE;

    if (!app_data->projectm && !milkdrop_try_init_projectm(app_data))
        return TRUE;

    if (!app_data->projectm || !app_data->offscreen || !app_data->playlist)
        return TRUE;

    if (!offscreen_renderer_make_current(app_data->offscreen))
        return TRUE;

    if (atomic_exchange(&app_data->preset_dir_pending, false)) {
        char next_dir[sizeof(app_data->pending_preset_dir)] = {0};

        g_mutex_lock(&app_data->preset_dir_lock);
        g_strlcpy(next_dir, app_data->pending_preset_dir, sizeof(next_dir));
        g_mutex_unlock(&app_data->preset_dir_lock);

        if (next_dir[0] == '\0' || !g_file_test(next_dir, G_FILE_TEST_IS_DIR)) {
            g_warning("Rejected preset-dir change to invalid path: %s", next_dir);
        } else {
            g_mutex_lock(&app_data->preset_dir_lock);
            g_clear_pointer(&app_data->preset_dir, g_free);
            app_data->preset_dir = g_strdup(next_dir);
            g_mutex_unlock(&app_data->preset_dir_lock);
            milkdrop_sync_playlist_from_preset_dir(app_data);
        }
    }

    {
        bool do_next = atomic_exchange(&app_data->next_preset_pending, false);
        bool do_prev = atomic_exchange(&app_data->prev_preset_pending, false);

        if (do_next)
            projectm_playlist_play_next(app_data->playlist, true);
        else if (do_prev)
            projectm_playlist_play_previous(app_data->playlist, true);
    }

    if (app_data->render_width <= 0 || app_data->render_height <= 0) {
        if (app_data->verbose) {
            g_message("render: skipping - dimensions not ready (%dx%d)",
                      app_data->render_width, app_data->render_height);
        }
        return TRUE;
    }

    if (app_data->render_width != app_data->last_synced_render_width ||
        app_data->render_height != app_data->last_synced_render_height) {
        projectm_set_window_size(app_data->projectm,
                                 (size_t)app_data->render_width,
                                 (size_t)app_data->render_height);
        app_data->last_synced_render_width = app_data->render_width;
        app_data->last_synced_render_height = app_data->render_height;
    }

    renderer_frame_prep(app_data,
                        app_data->pcm_render_buf,
                        G_N_ELEMENTS(app_data->pcm_render_buf),
                        &prep);
    if (!prep.would_draw)
        return TRUE;

    if (app_data->pm_mono_origin_us == 0)
        app_data->pm_mono_origin_us = g_get_monotonic_time();

    projectm_set_frame_time(app_data->projectm,
                            (g_get_monotonic_time() - app_data->pm_mono_origin_us) / 1000000.0);

    if (prep.would_feed_pcm) {
        guint frames = prep.stereo_frames;
        if (app_data->pcm_max_samples_per_channel > 0 &&
            frames > app_data->pcm_max_samples_per_channel) {
            frames = app_data->pcm_max_samples_per_channel;
        }
        projectm_pcm_add_float(app_data->projectm,
                               app_data->pcm_render_buf,
                               frames,
                               PROJECTM_STEREO);
    }

    SYNC_ATOMIC(shuffle_runtime, last_synced_shuffle,
                projectm_playlist_set_shuffle(app_data->playlist, _cur));

    SYNC_ATOMIC(preset_rotation_interval, last_synced_interval,
                projectm_set_preset_duration(app_data->projectm, (double)_cur));

    SYNC_ATOMIC(transitions.beat_sensitivity, transitions.last_synced_beat_sensitivity,
                projectm_set_beat_sensitivity(app_data->projectm, _cur));

    SYNC_ATOMIC(transitions.hard_cut_enabled, transitions.last_synced_hard_cut_enabled,
                projectm_set_hard_cut_enabled(app_data->projectm, _cur));

    SYNC_ATOMIC(transitions.hard_cut_sensitivity, transitions.last_synced_hard_cut_sensitivity,
                projectm_set_hard_cut_sensitivity(app_data->projectm, _cur));

    SYNC_ATOMIC(transitions.hard_cut_duration, transitions.last_synced_hard_cut_duration,
                projectm_set_hard_cut_duration(app_data->projectm, _cur));

    SYNC_ATOMIC(transitions.soft_cut_duration, transitions.last_synced_soft_cut_duration,
                projectm_set_soft_cut_duration(app_data->projectm, _cur));

    {
        (void)renderer_measure_render_fps(app_data, g_get_monotonic_time());
        int target_fps = atomic_load(&app_data->fps_runtime);
        if (target_fps <= 0)
            target_fps = 60;

        if (target_fps != app_data->last_synced_projectm_fps) {
            projectm_set_fps(app_data->projectm, target_fps);
            app_data->last_synced_projectm_fps = target_fps;
        }
    }

    if (!offscreen_renderer_begin_frame(app_data->offscreen,
                                        app_data->render_width,
                                        app_data->render_height,
                                        &target_fbo)) {
        return TRUE;
    }

    /* Deferred until after at least one projectm_opengl_render_frame_fbo call:
     * Mesa's Gallium shader compiler dereferences NULL when projectm_load_preset_file
     * is invoked before the first render cycle (confirmed by coredumps, signal 11
     * inside libgallium shader compiler). render_frame_counter == 0 means no prior
     * render cycle has completed; skip until next tick. */
    if (app_data->render_frame_counter > 0 &&
        atomic_exchange(&app_data->restore_state_pending, false)) {
        char restore_path[MILKDROP_PATH_MAX];
        bool restore_paused = false;
        g_mutex_lock(&app_data->quarantine.last_preset_lock);
        g_strlcpy(restore_path, app_data->restore_preset_path, sizeof(restore_path));
        restore_paused = app_data->restore_preset_paused;
        g_mutex_unlock(&app_data->quarantine.last_preset_lock);
        if (restore_path[0] != '\0') {
            if (restore_paused)
                atomic_store(&app_data->pause_audio, true);
            glFinish();
            projectm_load_preset_file(app_data->projectm, restore_path, false);
        }
    }

    projectm_opengl_render_frame_fbo(app_data->projectm, target_fbo);
    offscreen_renderer_finish_gpu(app_data->offscreen);

    if (app_data->verbose) {
        g_message("render: calling read_rgba, dims=%dx%d, fbo=%u",
                  app_data->render_width, app_data->render_height, target_fbo);
    }

    if (!offscreen_renderer_read_rgba(app_data->offscreen, &pixels, &pixels_len, &stride)) {
        if (app_data->verbose) {
            g_warning("render: read_rgba FAILED, dims=%dx%d, fbo=%u",
                      app_data->render_width, app_data->render_height, target_fbo);
        }
        return TRUE;
    }

    if (atomic_exchange(&app_data->screenshot.requested, false)) {
        char local_screenshot_path[MILKDROP_PATH_MAX];

        g_mutex_lock(&app_data->screenshot.lock);
        g_strlcpy(local_screenshot_path,
                  app_data->screenshot.path,
                  sizeof(local_screenshot_path));
        app_data->screenshot.path[0] = '\0';
        g_mutex_unlock(&app_data->screenshot.lock);

        if (local_screenshot_path[0] != '\0') {
            milkdrop_save_screenshot_rgba(local_screenshot_path,
                                          pixels,
                                          app_data->render_width,
                                          app_data->render_height,
                                          stride);
            g_message("screenshot: saved to %s", local_screenshot_path);
        }
    }

    milkdrop_publish_frame_to_picture(app_data, pixels, pixels_len, stride);
    app_data->render_frame_counter++;
    return TRUE;
#else
    (void)app_data;
    return TRUE;
#endif
}

static gboolean
on_close_request(GtkWidget* widget, gpointer user_data)
{
    (void)widget;
    (void)user_data;
    return GDK_EVENT_STOP;
}

static void
milkdrop_set_initial_title(GtkWindow* window,
                           int        monitor,
                           gboolean   overlay,
                           double     opacity)
{
    char title[256];
    int opacity_percent = (int)(opacity * 100.0 + 0.5);
    if (opacity_percent < 0)
        opacity_percent = 0;
    if (opacity_percent > 100)
        opacity_percent = 100;

    snprintf(title, sizeof(title),
             "@milkdrop!{\"monitor\":%d,\"overlay\":%s,\"opacity\":%d}|%d",
             monitor,
             overlay ? "true" : "false",
             opacity_percent,
             monitor);
    gtk_window_set_title(window, title);
    g_message("Window title set: %s", title);
}

static void
build_window(AppData* app_data)
{
    GtkWidget* window = gtk_application_window_new(app_data->app);
    GtkWidget* picture = gtk_picture_new();

    app_data->window = GTK_WINDOW(window);
    app_data->picture = GTK_PICTURE(picture);

    milkdrop_set_initial_title(app_data->window,
                               app_data->monitor_index,
                               atomic_load(&app_data->overlay_enabled),
                               (double)atomic_load(&app_data->opacity));

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
    app_data->render_width = default_width;
    app_data->render_height = default_height;
    app_data->initial_width = default_width;
    app_data->initial_height = default_height;
    gtk_window_set_decorated(app_data->window, false);
    gtk_window_set_resizable(app_data->window, false);

    gtk_widget_set_focusable(GTK_WIDGET(app_data->window), FALSE);
    gtk_widget_set_can_target(picture, FALSE);
    gtk_widget_set_focusable(picture, FALSE);
    gtk_picture_set_can_shrink(app_data->picture, TRUE);
    gtk_picture_set_content_fit(app_data->picture, GTK_CONTENT_FIT_FILL);

    gtk_window_set_child(app_data->window, picture);

    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), app_data);

    g_signal_connect_after(window, "map", G_CALLBACK(on_window_map), NULL);

    app_data->render_pulse_source_id =
        g_timeout_add(MILKDROP_RENDER_PULSE_INTERVAL_MS, on_render_pulse, app_data);
    gtk_window_present(app_data->window);
    gtk_widget_set_opacity(GTK_WIDGET(app_data->window),
                           (float)atomic_load(&app_data->opacity));

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

#if HAVE_PROJECTM
    milkdrop_unregister_projectm_logging();
    if (app_data->playlist) {
        projectm_playlist_destroy(app_data->playlist);
        app_data->playlist = NULL;
    }
    if (app_data->projectm) {
        projectm_destroy(app_data->projectm);
        app_data->projectm = NULL;
    }
    if (app_data->offscreen) {
        offscreen_renderer_free(app_data->offscreen);
        app_data->offscreen = NULL;
    }
    offscreen_renderer_global_shutdown();
#endif

    g_mutex_lock(&app_data->preset_dir_lock);
    g_clear_pointer(&app_data->preset_dir, g_free);
    g_mutex_unlock(&app_data->preset_dir_lock);
    g_clear_pointer(&app_data->textures_dir, g_free);
    g_clear_pointer(&app_data->socket_path, g_free);
    g_mutex_clear(&app_data->preset_dir_lock);
    g_mutex_clear(&app_data->screenshot.lock);
    g_mutex_clear(&app_data->quarantine.last_preset_lock);
}

int
main(int argc, char** argv)
{
    /* Heap-allocate: AppData is large due to quarantine_list. */
    AppData* app_data = g_new0(AppData, 1);
    int monitor_index = 0;
    double opacity = 1.0;
    gboolean shuffle = FALSE;
    gboolean overlay = FALSE;
    gboolean verbose = FALSE;
    char* preset_dir = NULL;
    char* socket_path = NULL;
    int preset_rotation_interval = 30;
    int fps_cli = 0;
    double beat_sensitivity_cli = 1.0;
    gboolean hard_cut_enabled_cli = FALSE;
    double hard_cut_sensitivity_cli = 2.0;
    double hard_cut_duration_cli = 20.0;
    double soft_cut_duration_cli = 3.0;

    GOptionEntry entries[] = {
        {"monitor", 0, 0, G_OPTION_ARG_INT, &monitor_index, "Monitor index", "INDEX"},
        {"opacity", 0, 0, G_OPTION_ARG_DOUBLE, &opacity, "Window opacity (0..1)", "VALUE"},
        {"preset-dir", 0, 0, G_OPTION_ARG_FILENAME, &preset_dir, "Preset directory", "PATH"},
        {"socket-path", 0, 0, G_OPTION_ARG_FILENAME, &socket_path, "Control socket path", "PATH"},
        {"shuffle", 0, 0, G_OPTION_ARG_NONE, &shuffle, "Enable shuffle mode", NULL},
        {"overlay", 0, 0, G_OPTION_ARG_NONE, &overlay, "Enable overlay mode (state + control socket)", NULL},
        {"verbose", 0, 0, G_OPTION_ARG_NONE, &verbose, "Verbose logging", NULL},
        {"preset-rotation-interval", 0, 0, G_OPTION_ARG_INT, &preset_rotation_interval, "Preset rotation interval in seconds", "SECONDS"},
        {"fps", 0, 0, G_OPTION_ARG_INT, &fps_cli, "Target frame rate (10-144; default 60)", "FPS"},
        {"beat-sensitivity", 0, 0, G_OPTION_ARG_DOUBLE, &beat_sensitivity_cli, "Beat sensitivity (0.0-5.0; default 1.0)", "VALUE"},
        {"hard-cut-enabled", 0, 0, G_OPTION_ARG_NONE, &hard_cut_enabled_cli, "Enable hard preset cuts on strong beats", NULL},
        {"hard-cut-sensitivity", 0, 0, G_OPTION_ARG_DOUBLE, &hard_cut_sensitivity_cli, "Hard cut sensitivity (0.0-5.0; default 2.0)", "VALUE"},
        {"hard-cut-duration", 0, 0, G_OPTION_ARG_DOUBLE, &hard_cut_duration_cli, "Min seconds between hard cuts (1-120; default 20)", "SECONDS"},
        {"soft-cut-duration", 0, 0, G_OPTION_ARG_DOUBLE, &soft_cut_duration_cli, "Soft cut cross-fade duration (1-30; default 3)", "SECONDS"},
        {NULL},
    };

    GOptionContext* context = g_option_context_new("- milkdrop renderer");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, NULL)) {
        g_printerr("Failed to parse command line arguments\n");
        g_option_context_free(context);
        g_free(app_data);
        return 1;
    }
    g_option_context_free(context);

    g_set_prgname("milkdrop");

    g_mutex_init(&app_data->preset_dir_lock);

    app_data->verbose = verbose;
#if HAVE_PROJECTM
    if (!offscreen_renderer_preinit())
        g_warning("offscreen: SDL preinit failed before GTK startup");
    atomic_store(&app_data->projectm_init_aborted, false);
#endif

    app_data->monitor_index = monitor_index;
    g_mutex_lock(&app_data->preset_dir_lock);
    app_data->preset_dir = preset_dir;
    g_mutex_unlock(&app_data->preset_dir_lock);
    app_data->socket_path = socket_path;
    app_data->shuffle = (bool)shuffle;
    atomic_store(&app_data->opacity, (float)CLAMP(opacity, 0.0, 1.0));
    atomic_store(&app_data->pause_audio, false);
    atomic_store(&app_data->shutdown_requested, false);
    atomic_store(&app_data->overlay_enabled, overlay);
    atomic_store(&app_data->shuffle_runtime, app_data->shuffle);
    atomic_store(&app_data->preset_dir_pending, false);
    atomic_store(&app_data->next_preset_pending, false);
    atomic_store(&app_data->prev_preset_pending, false);
    app_data->restore_preset_path[0] = '\0';
    app_data->restore_preset_paused = false;
    atomic_store(&app_data->restore_state_pending, false);
    {
        int fps_init = 60;
        if (fps_cli >= 10 && fps_cli <= 144)
            fps_init = fps_cli;
        atomic_store(&app_data->fps_runtime, fps_init);
    }
    atomic_store(&app_data->fps_last, 0.0f);
    /* -1 forces first on_render_pulse to install the correct interval. */
    app_data->fps_applied = -1;
    app_data->fps_last_render_us = 0;
    app_data->last_synced_projectm_fps = -1;
    atomic_store(&app_data->preset_rotation_interval, CLAMP(preset_rotation_interval, 5, 300));
    atomic_store(&app_data->transitions.beat_sensitivity, (float)CLAMP(beat_sensitivity_cli, 0.0, 5.0));
    atomic_store(&app_data->transitions.hard_cut_enabled, (bool)hard_cut_enabled_cli);
    atomic_store(&app_data->transitions.hard_cut_sensitivity, (float)CLAMP(hard_cut_sensitivity_cli, 0.0, 5.0));
    atomic_store(&app_data->transitions.hard_cut_duration, CLAMP(hard_cut_duration_cli, 1.0, 120.0));
    atomic_store(&app_data->transitions.soft_cut_duration, CLAMP(soft_cut_duration_cli, 1.0, 30.0));
    atomic_store(&app_data->control_thread_running, false);
    app_data->control_fd = -1;
    app_data->control_thread = NULL;
    g_mutex_init(&app_data->screenshot.lock);
    g_mutex_init(&app_data->quarantine.last_preset_lock);
    app_data->pending_preset_dir[0] = '\0';

    GtkApplication* app = gtk_application_new(
      NULL,
      G_APPLICATION_NON_UNIQUE
    );

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), app_data);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), app_data);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    g_free(app_data);
    return status;
}
