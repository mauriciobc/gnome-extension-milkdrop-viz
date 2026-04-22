#pragma once

#include <glib.h>
#include <stdint.h>

typedef struct _OffscreenRenderer OffscreenRenderer;

gboolean offscreen_renderer_preinit(void);
void     offscreen_renderer_global_shutdown(void);

OffscreenRenderer* offscreen_renderer_new(void);
void               offscreen_renderer_free(OffscreenRenderer* renderer);

gboolean offscreen_renderer_init(OffscreenRenderer* renderer,
                                 int                width,
                                 int                height,
                                 gboolean           verbose);
void     offscreen_renderer_shutdown(OffscreenRenderer* renderer);

gboolean offscreen_renderer_make_current(OffscreenRenderer* renderer);
void*    offscreen_renderer_gl_load_proc(const char* name,
                                         void*       user_data);

gboolean offscreen_renderer_begin_frame(OffscreenRenderer* renderer,
                                        int                width,
                                        int                height,
                                        uint32_t*          out_fbo);
void     offscreen_renderer_finish_gpu(OffscreenRenderer* renderer);

gboolean offscreen_renderer_read_rgba(OffscreenRenderer* renderer,
                                      guint8**           out_pixels,
                                      gsize*             out_len,
                                      gsize*             out_stride);
