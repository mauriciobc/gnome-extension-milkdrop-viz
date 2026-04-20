#include "renderer.h"
#include "app.h"

#include <glib.h>

#if HAVE_PROJECTM
#include <projectM-4/render_opengl.h>
#include <projectM-4/parameters.h>
#endif

void
renderer_apply_resize(AppData* app_data,
                      int      widget_width,
                      int      widget_height,
                      int      scale_factor)
{
    if (!app_data)
        return;

    app_data->render_width  = widget_width  * scale_factor;
    app_data->render_height = widget_height * scale_factor;

    if (app_data->verbose) {
        g_message("resize: widget=%dx%d, scale=%d, render=%dx%d",
                  widget_width, widget_height, scale_factor,
                  app_data->render_width, app_data->render_height);
    }

#if HAVE_PROJECTM
    if (app_data->projectm)
        projectm_set_window_size(app_data->projectm,
                                 (size_t)app_data->render_width,
                                 (size_t)app_data->render_height);
#endif
}

void
renderer_frame_prep(AppData*           app,
                    float*             pcm_buf,
                    size_t             pcm_cap,
                    RendererFramePrep* out)
{
    if (!out)
        return;

    out->floats_copied   = 0;
    out->stereo_frames   = 0;
    out->would_feed_pcm  = FALSE;
    out->would_draw      = FALSE;

    if (!app)
        return;

#if HAVE_PROJECTM
    if (app->projectm && app->render_width > 0 && app->render_height > 0) {
        out->would_draw = TRUE;
        if (!pcm_buf || pcm_cap == 0)
            return;

        out->floats_copied = audio_ring_read(&app->ring, pcm_buf, pcm_cap);

        if (!atomic_load(&app->pause_audio) && out->floats_copied >= 2) {
            out->would_feed_pcm = TRUE;
            out->stereo_frames  = (guint)(out->floats_copied / 2);
        }
    }
#else
    (void)pcm_buf;
    (void)pcm_cap;
#endif
}

int
renderer_measure_render_fps(AppData* app,
                            gint64   frame_time_us)
{
    if (!app)
        return 60;

    int target_fps = atomic_load(&app->fps_runtime);
    if (target_fps <= 0)
        target_fps = 60;

    if (frame_time_us <= 0)
        return target_fps;

    if (app->fps_last_render_us > 0) {
        gint64 delta_us = frame_time_us - app->fps_last_render_us;
        if (delta_us > 0) {
            float measured = (float)(1000000.0 / (double)delta_us);
            atomic_store(&app->fps_last, measured);

            int measured_int = (int)(measured + 0.5f);
            if (measured_int < 1)
                measured_int = 1;
            app->fps_last_render_us = frame_time_us;
            return measured_int;
        }
    }

    app->fps_last_render_us = frame_time_us;
    return target_fps;
}
