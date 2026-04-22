#include <glib.h>
#include <string.h>
#include <epoxy/gl.h>

#include "offscreen_renderer.h"

#define TEST_WIDTH 64
#define TEST_HEIGHT 64

static void
test_offscreen_preinit(void)
{
    g_assert_true(offscreen_renderer_preinit());
}

static void
test_offscreen_lifecycle(void)
{
    OffscreenRenderer* renderer = offscreen_renderer_new();
    g_assert_nonnull(renderer);

    g_assert_true(offscreen_renderer_init(renderer, TEST_WIDTH, TEST_HEIGHT, FALSE));
    offscreen_renderer_shutdown(renderer);
    offscreen_renderer_free(renderer);
}

static void
test_offscreen_render_and_readback(void)
{
    OffscreenRenderer* renderer = offscreen_renderer_new();
    g_assert_nonnull(renderer);
    g_assert_true(offscreen_renderer_init(renderer, TEST_WIDTH, TEST_HEIGHT, FALSE));

    uint32_t fbo = 0;
    g_assert_true(offscreen_renderer_begin_frame(renderer, TEST_WIDTH, TEST_HEIGHT, &fbo));
    g_assert_cmpuint(fbo, >, 0);

    /* Clear to a distinctive color so we can verify readback. */
    glClearColor(0.8f, 0.2f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    offscreen_renderer_finish_gpu(renderer);

    guint8* pixels = NULL;
    gsize len = 0;
    gsize stride = 0;
    g_assert_true(offscreen_renderer_read_rgba(renderer, &pixels, &len, &stride));
    g_assert_nonnull(pixels);
    g_assert_cmpuint(len, ==, (gsize)TEST_WIDTH * TEST_HEIGHT * 4u);
    g_assert_cmpuint(stride, ==, (gsize)TEST_WIDTH * 4u);

    /* Probe the center pixel. */
    int cx = TEST_WIDTH / 2;
    int cy = TEST_HEIGHT / 2;
    gsize offset = (gsize)cy * stride + (gsize)cx * 4u;

    guint8 r = pixels[offset + 0];
    guint8 g = pixels[offset + 1];
    guint8 b = pixels[offset + 2];
    guint8 a = pixels[offset + 3];

    /* Allow generous tolerance for driver color-space/quantization differences. */
    g_assert_cmpuint(r, >, 180);
    g_assert_cmpuint(g, <, 80);
    g_assert_cmpuint(b, >, 80);
    g_assert_cmpuint(b, <, 180);
    g_assert_cmpuint(a, >, 200);

    g_free(pixels);
    offscreen_renderer_shutdown(renderer);
    offscreen_renderer_free(renderer);
}

static void
test_offscreen_resize_recreate_target(void)
{
    OffscreenRenderer* renderer = offscreen_renderer_new();
    g_assert_nonnull(renderer);
    g_assert_true(offscreen_renderer_init(renderer, 32, 32, FALSE));

    uint32_t fbo1 = 0;
    g_assert_true(offscreen_renderer_begin_frame(renderer, 32, 32, &fbo1));

    /* Resize to a larger target; begin_frame should recreate resources. */
    uint32_t fbo2 = 0;
    g_assert_true(offscreen_renderer_begin_frame(renderer, 128, 128, &fbo2));
    g_assert_cmpuint(fbo2, >, 0);

    offscreen_renderer_finish_gpu(renderer);

    guint8* pixels = NULL;
    gsize len = 0;
    gsize stride = 0;
    g_assert_true(offscreen_renderer_read_rgba(renderer, &pixels, &len, &stride));
    g_assert_cmpuint(len, ==, 128u * 128u * 4u);
    g_free(pixels);

    offscreen_renderer_shutdown(renderer);
    offscreen_renderer_free(renderer);
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/offscreen/preinit", test_offscreen_preinit);
    g_test_add_func("/offscreen/lifecycle", test_offscreen_lifecycle);
    g_test_add_func("/offscreen/render-and-readback", test_offscreen_render_and_readback);
    g_test_add_func("/offscreen/resize-recreate-target", test_offscreen_resize_recreate_target);

    return g_test_run();
}
