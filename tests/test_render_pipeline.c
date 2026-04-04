#include <glib.h>

#include "app.h"
#include "renderer.h"

#if HAVE_PROJECTM
static void
test_prep_empty_ring(void)
{
    AppData           app = {0};
    float             pcm[1024];
    RendererFramePrep prep;

    audio_ring_init(&app.ring);
    app.render_width  = 1280;
    app.render_height = 720;
    app.projectm      = (void*)0x1;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_true(prep.would_draw);
    g_assert_cmpuint((guint)prep.floats_copied, ==, 0u);
    g_assert_false(prep.would_feed_pcm);
}

static void
test_prep_one_float_no_pcm_feed(void)
{
    AppData           app = {0};
    float             pcm[1024];
    RendererFramePrep prep;
    float             one = 1.0f;

    audio_ring_init(&app.ring);
    audio_ring_push(&app.ring, &one, 1);
    app.render_width  = 1280;
    app.render_height = 720;
    app.projectm      = (void*)0x1;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_true(prep.would_draw);
    g_assert_cmpuint((guint)prep.floats_copied, ==, 1u);
    g_assert_false(prep.would_feed_pcm);
}

static void
test_prep_four_floats_stereo(void)
{
    AppData           app = {0};
    float             pcm[1024];
    RendererFramePrep prep;
    float             in[4] = {0.25f, -0.5f, 0.75f, 1.0f};

    audio_ring_init(&app.ring);
    audio_ring_push(&app.ring, in, G_N_ELEMENTS(in));
    app.render_width  = 1280;
    app.render_height = 720;
    app.projectm      = (void*)0x1;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_true(prep.would_draw);
    g_assert_cmpuint((guint)prep.floats_copied, ==, 4u);
    g_assert_true(prep.would_feed_pcm);
    g_assert_cmpuint(prep.stereo_frames, ==, 2u);
    g_assert_cmpfloat(pcm[0], ==, 0.25f);
    g_assert_cmpfloat(pcm[1], ==, -0.5f);
    g_assert_cmpfloat(pcm[2], ==, 0.75f);
    g_assert_cmpfloat(pcm[3], ==, 1.0f);
}

static void
test_prep_pause_skips_feed(void)
{
    AppData           app = {0};
    float             pcm[1024];
    RendererFramePrep prep;
    float             in[4] = {1.0f, 2.0f, 3.0f, 4.0f};

    audio_ring_init(&app.ring);
    audio_ring_push(&app.ring, in, G_N_ELEMENTS(in));
    atomic_store(&app.pause_audio, true);
    app.render_width  = 1280;
    app.render_height = 720;
    app.projectm      = (void*)0x1;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_true(prep.would_draw);
    g_assert_cmpuint((guint)prep.floats_copied, ==, 4u);
    g_assert_false(prep.would_feed_pcm);
}

static void
test_prep_zero_width_no_draw(void)
{
    AppData           app = {0};
    float             pcm[1024];
    RendererFramePrep prep;

    audio_ring_init(&app.ring);
    app.render_width  = 0;
    app.render_height = 720;
    app.projectm      = (void*)0x1;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_false(prep.would_draw);
}

static void
test_prep_zero_height_no_draw(void)
{
    AppData           app = {0};
    float             pcm[1024];
    RendererFramePrep prep;

    audio_ring_init(&app.ring);
    app.render_width  = 1280;
    app.render_height = 0;
    app.projectm      = (void*)0x1;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_false(prep.would_draw);
}

static void
test_prep_null_projectm_no_draw(void)
{
    AppData           app = {0};
    float             pcm[1024];
    RendererFramePrep prep;

    audio_ring_init(&app.ring);
    app.render_width  = 1280;
    app.render_height = 720;
    app.projectm      = NULL;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_false(prep.would_draw);
}

static void
test_prep_drain_large_ring(void)
{
    AppData           app = {0};
    float             pcm[2048];
    RendererFramePrep prep;
    float             in[2048];

    for (int i = 0; i < 2048; i++)
        in[i] = (float)i * 0.001f;

    audio_ring_init(&app.ring);
    audio_ring_push(&app.ring, in, G_N_ELEMENTS(in));
    app.render_width  = 1280;
    app.render_height = 720;
    app.projectm      = (void*)0x1;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_true(prep.would_draw);
    g_assert_cmpuint((guint)prep.floats_copied, ==, 2048u);
    g_assert_true(prep.would_feed_pcm);
    g_assert_cmpuint(prep.stereo_frames, ==, 1024u);
}

static void
test_prep_ring_not_read_when_no_draw(void)
{
    AppData           app = {0};
    float             pcm[1024];
    RendererFramePrep prep;
    float             in[4] = {9.0f, 8.0f, 7.0f, 6.0f};

    audio_ring_init(&app.ring);
    audio_ring_push(&app.ring, in, G_N_ELEMENTS(in));
    app.render_width  = 0;
    app.render_height = 720;
    app.projectm      = (void*)0x1;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);
    g_assert_false(prep.would_draw);

    app.render_width  = 1280;
    app.render_height = 720;
    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_true(prep.would_draw);
    g_assert_cmpuint((guint)prep.floats_copied, ==, 4u);
    g_assert_cmpfloat(pcm[0], ==, 9.0f);
}
#endif /* HAVE_PROJECTM */

static void
test_prep_null_out_silent(void)
{
    AppData app = {0};
    float   pcm[4];

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), NULL);
}

#if !HAVE_PROJECTM
static void
test_prep_no_projectm_build(void)
{
    AppData           app = {0};
    float             pcm[1024];
    RendererFramePrep prep;

    audio_ring_init(&app.ring);
    float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    audio_ring_push(&app.ring, in, G_N_ELEMENTS(in));
    app.render_width  = 1280;
    app.render_height = 720;
    app.projectm      = (void*)0x1;

    renderer_frame_prep(&app, pcm, G_N_ELEMENTS(pcm), &prep);

    g_assert_false(prep.would_draw);
    g_assert_false(prep.would_feed_pcm);
    g_assert_cmpuint((guint)prep.floats_copied, ==, 0u);
}
#endif

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    /* These tests do not touch GtkGLArea, projectm_opengl_render_frame, or the Shell
     * compositor — green here does not mean wallpaper/video is visible on the desktop. */
    g_test_message(
        "render-pipeline: logic-only suite (ring prep); no GL readback or Mutter checks");

    g_test_add_func("/render-pipeline/prep-null-out-silent", test_prep_null_out_silent);

#if HAVE_PROJECTM
    g_test_add_func("/render-pipeline/prep-empty-ring", test_prep_empty_ring);
    g_test_add_func("/render-pipeline/prep-one-float", test_prep_one_float_no_pcm_feed);
    g_test_add_func("/render-pipeline/prep-four-floats-stereo", test_prep_four_floats_stereo);
    g_test_add_func("/render-pipeline/prep-pause", test_prep_pause_skips_feed);
    g_test_add_func("/render-pipeline/prep-zero-width", test_prep_zero_width_no_draw);
    g_test_add_func("/render-pipeline/prep-zero-height", test_prep_zero_height_no_draw);
    g_test_add_func("/render-pipeline/prep-null-projectm", test_prep_null_projectm_no_draw);
    g_test_add_func("/render-pipeline/prep-ring-not-read-without-draw", test_prep_ring_not_read_when_no_draw);
    g_test_add_func("/render-pipeline/prep-drain-large-ring", test_prep_drain_large_ring);
#else
    g_test_add_func("/render-pipeline/prep-without-projectm-build", test_prep_no_projectm_build);
#endif

    return g_test_run();
}
