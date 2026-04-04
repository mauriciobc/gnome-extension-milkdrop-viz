#include <glib.h>

#include "app.h"

static void
test_ring_init(void)
{
    AudioRing ring = {0};
    audio_ring_init(&ring);

    g_assert_cmpuint(atomic_load(&ring.read_index), ==, 0u);
    g_assert_cmpuint(atomic_load(&ring.write_index), ==, 0u);
}

static void
test_ring_push_read_roundtrip(void)
{
    AudioRing ring = {0};
    float in[8] = {0.0f, 0.25f, 0.5f, 0.75f, -0.25f, -0.5f, -0.75f, 1.0f};
    float out[8] = {0};

    audio_ring_init(&ring);
    g_assert_cmpuint(audio_ring_push(&ring, in, G_N_ELEMENTS(in)), ==, G_N_ELEMENTS(in));
    g_assert_cmpuint(audio_ring_read(&ring, out, G_N_ELEMENTS(out)), ==, G_N_ELEMENTS(out));

    for (guint i = 0; i < G_N_ELEMENTS(in); i++)
        g_assert_cmpfloat(out[i], ==, in[i]);
}

static void
test_ring_capacity_limit(void)
{
    AudioRing ring = {0};
    float value = 0.1f;

    audio_ring_init(&ring);

    for (guint i = 0; i < MILKDROP_RING_CAPACITY - 1u; i++) {
        g_assert_cmpuint(audio_ring_push(&ring, &value, 1), ==, 1u);
    }

    g_assert_cmpuint(audio_ring_push(&ring, &value, 1), ==, 0u);
}

static void
test_ring_wraparound(void)
{
    AudioRing ring = {0};
    float in1[6] = {1, 2, 3, 4, 5, 6};
    float in2[6] = {7, 8, 9, 10, 11, 12};
    float out[12] = {0};

    audio_ring_init(&ring);
    g_assert_cmpuint(audio_ring_push(&ring, in1, 6), ==, 6u);
    g_assert_cmpuint(audio_ring_read(&ring, out, 4), ==, 4u);
    g_assert_cmpuint(audio_ring_push(&ring, in2, 6), ==, 6u);
    g_assert_cmpuint(audio_ring_read(&ring, out, G_N_ELEMENTS(out)), ==, 8u);

    g_assert_cmpfloat(out[0], ==, 5.0f);
    g_assert_cmpfloat(out[1], ==, 6.0f);
    g_assert_cmpfloat(out[2], ==, 7.0f);
    g_assert_cmpfloat(out[3], ==, 8.0f);
    g_assert_cmpfloat(out[4], ==, 9.0f);
    g_assert_cmpfloat(out[5], ==, 10.0f);
    g_assert_cmpfloat(out[6], ==, 11.0f);
    g_assert_cmpfloat(out[7], ==, 12.0f);
}

static void
test_ring_null_and_empty(void)
{
    AudioRing ring = {0};
    float in = 0.2f;
    float out = 0.0f;

    audio_ring_init(&ring);

    g_assert_cmpuint(audio_ring_push(&ring, NULL, 1), ==, 0u);
    g_assert_cmpuint(audio_ring_push(&ring, &in, 0), ==, 0u);

    g_assert_cmpuint(audio_ring_read(&ring, NULL, 1), ==, 0u);
    g_assert_cmpuint(audio_ring_read(&ring, &out, 0), ==, 0u);
    g_assert_cmpuint(audio_ring_read(&ring, &out, 1), ==, 0u);
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ring/init", test_ring_init);
    g_test_add_func("/ring/push-read-roundtrip", test_ring_push_read_roundtrip);
    g_test_add_func("/ring/capacity-limit", test_ring_capacity_limit);
    g_test_add_func("/ring/wraparound", test_ring_wraparound);
    g_test_add_func("/ring/null-and-empty", test_ring_null_and_empty);

    return g_test_run();
}