#include <glib.h>

#include "app.h"
#include "audio.h"

static void
test_align_stereo_float_count(void)
{
    g_assert_cmpuint((guint)audio_align_stereo_float_count(0), ==, 0u);
    g_assert_cmpuint((guint)audio_align_stereo_float_count(1), ==, 0u);
    g_assert_cmpuint((guint)audio_align_stereo_float_count(2), ==, 2u);
    g_assert_cmpuint((guint)audio_align_stereo_float_count(3), ==, 2u);
    g_assert_cmpuint((guint)audio_align_stereo_float_count(4), ==, 4u);
}

static void
test_stereo_alignment_odd_count(void)
{
    AudioRing ring = {0};
    audio_ring_init(&ring);

    float in[5] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    size_t pushed = audio_ring_push(&ring, in, 5);

    g_assert_cmpuint(pushed, ==, 5u);

    float out[5] = {0};
    size_t read = audio_ring_read(&ring, out, 5);
    g_assert_cmpuint(read, ==, 5u);
    g_assert_cmpfloat(out[0], ==, 1.0f);
    g_assert_cmpfloat(out[1], ==, 2.0f);
    g_assert_cmpfloat(out[2], ==, 3.0f);
    g_assert_cmpfloat(out[3], ==, 4.0f);
    g_assert_cmpfloat(out[4], ==, 5.0f);
}

static void
test_stereo_alignment_even_count(void)
{
    AudioRing ring = {0};
    audio_ring_init(&ring);

    float in[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    size_t pushed = audio_ring_push(&ring, in, 6);

    g_assert_cmpuint(pushed, ==, 6u);

    float out[6] = {0};
    size_t read = audio_ring_read(&ring, out, 6);
    g_assert_cmpuint(read, ==, 6u);
}

static void
test_stereo_alignment_zero_count(void)
{
    AudioRing ring = {0};
    audio_ring_init(&ring);

    float in[2] = {1.0f, 2.0f};
    size_t pushed = audio_ring_push(&ring, in, 0);

    g_assert_cmpuint(pushed, ==, 0u);

    float out[2] = {0};
    size_t read = audio_ring_read(&ring, out, 2);
    g_assert_cmpuint(read, ==, 0u);
}

static void
test_continuous_push_ring_fill_behavior(void)
{
    AudioRing ring = {0};
    audio_ring_init(&ring);

    size_t total_pushed = 0;
    float chunk[256];
    for (guint i = 0; i < G_N_ELEMENTS(chunk); i++)
        chunk[i] = (float)i;

    while (audio_ring_push(&ring, chunk, G_N_ELEMENTS(chunk)) > 0) {
        total_pushed += G_N_ELEMENTS(chunk);
    }

    float drain[MILKDROP_RING_CAPACITY];
    size_t drained = audio_ring_read(&ring, drain, G_N_ELEMENTS(drain));

    g_assert_cmpuint(drained, ==, MILKDROP_RING_CAPACITY - 1u);

    float leftover[1] = {0};
    g_assert_cmpuint(audio_ring_read(&ring, leftover, 1), ==, 0u);
}

static void
test_push_excess_drops_samples(void)
{
    AudioRing ring = {0};
    audio_ring_init(&ring);

    float fill[MILKDROP_RING_CAPACITY - 1];
    for (guint i = 0; i < G_N_ELEMENTS(fill); i++)
        fill[i] = 1.0f;

    size_t pushed = audio_ring_push(&ring, fill, G_N_ELEMENTS(fill));
    g_assert_cmpuint(pushed, ==, MILKDROP_RING_CAPACITY - 1u);

    float extra[1] = {99.0f};
    g_assert_cmpuint(audio_ring_push(&ring, extra, 1), ==, 0u);

    float out[MILKDROP_RING_CAPACITY - 1];
    size_t read = audio_ring_read(&ring, out, G_N_ELEMENTS(out));
    g_assert_cmpuint(read, ==, MILKDROP_RING_CAPACITY - 1u);
    for (guint i = 0; i < read; i++)
        g_assert_cmpfloat(out[i], ==, 1.0f);
}

static void
test_partial_read_then_push(void)
{
    AudioRing ring = {0};
    audio_ring_init(&ring);

    float in[100];
    for (guint i = 0; i < G_N_ELEMENTS(in); i++)
        in[i] = (float)i;
    g_assert_cmpuint(audio_ring_push(&ring, in, G_N_ELEMENTS(in)), ==, 100u);

    float out[30] = {0};
    g_assert_cmpuint(audio_ring_read(&ring, out, 30), ==, 30u);
    for (guint i = 0; i < 30; i++)
        g_assert_cmpfloat(out[i], ==, (float)i);

    float more[50];
    for (guint i = 0; i < G_N_ELEMENTS(more); i++)
        more[i] = 100.0f + (float)i;
    g_assert_cmpuint(audio_ring_push(&ring, more, G_N_ELEMENTS(more)), ==, 50u);

    float drain[120] = {0};
    size_t read = audio_ring_read(&ring, drain, G_N_ELEMENTS(drain));
    g_assert_cmpuint(read, ==, 120u);

    for (guint i = 0; i < 70; i++)
        g_assert_cmpfloat(drain[i], ==, 30.0f + (float)i);

    for (guint i = 0; i < 50; i++)
        g_assert_cmpfloat(drain[70 + i], ==, 100.0f + (float)i);
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/audio/align-stereo-float-count", test_align_stereo_float_count);
    g_test_add_func("/audio/stereo-alignment-odd", test_stereo_alignment_odd_count);
    g_test_add_func("/audio/stereo-alignment-even", test_stereo_alignment_even_count);
    g_test_add_func("/audio/stereo-alignment-zero", test_stereo_alignment_zero_count);
    g_test_add_func("/audio/continuous-push-fill", test_continuous_push_ring_fill_behavior);
    g_test_add_func("/audio/push-excess-drops", test_push_excess_drops_samples);
    g_test_add_func("/audio/partial-read-then-push", test_partial_read_then_push);

    return g_test_run();
}
