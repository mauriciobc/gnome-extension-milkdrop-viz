#pragma once

#include "app.h"

/*
 * renderer_apply_resize - apply a widget resize to AppData and notify projectM.
 *
 * @app_data:       shared application state.
 * @widget_width:   new widget width in logical pixels.
 * @widget_height:  new widget height in logical pixels.
 * @scale_factor:   HiDPI scale factor (1 on standard displays, 2 on HiDPI).
 *
 * Sets app_data->render_width = widget_width * scale_factor and
 * app_data->render_height = widget_height * scale_factor, then calls
 * projectm_set_window_size() if a projectM handle exists.
 *
 * Exposed as a non-static function so unit tests can call it without a
 * live GtkGLArea (the GtkWidget scale-factor lookup happens in the
 * signal handler on_resize() which wraps this function).
 */
void renderer_apply_resize(AppData* app_data,
                           int      widget_width,
                           int      widget_height,
                           int      scale_factor);

/*
 * renderer_frame_prep - PCM read + flags for one render tick (mirrors on_render guards).
 *
 * Mutates @app->ring (consumer read). When would_draw is FALSE, the ring is not read.
 * @pcm_buf must be valid whenever a draw would occur; may be NULL only if would_draw
 * will be FALSE for the given @app state.
 */
typedef struct {
    size_t   floats_copied;
    guint    stereo_frames;
    gboolean would_feed_pcm;
    gboolean would_draw;
} RendererFramePrep;

void renderer_frame_prep(AppData*           app,
                         float*             pcm_buf,
                         size_t             pcm_cap,
                         RendererFramePrep* out);

/*
 * renderer_measure_render_fps - update the measured FPS from a completed render.
 *
 * Uses @frame_time_us as the timestamp of a real rendered frame, not just the
 * render pulse timer. Returns the FPS value projectM should see: measured FPS
 * once available, otherwise the configured target FPS as a startup fallback.
 */
int renderer_measure_render_fps(AppData* app,
                                gint64   frame_time_us);
