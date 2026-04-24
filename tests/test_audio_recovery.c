/**
 * test_audio_recovery.c
 *
 * Unit tests for the audio recovery state machine (audio_recovery.c).
 * Tests run without PipeWire — they only exercise the state tracking
 * functions that the stream state-change callback calls.
 */

#include <glib.h>

#include "app.h"
#include "audio_recovery.h"

/* ── tests ───────────────────────────────────────────────────────────────── */

static void
test_audio_fail_count_zero_on_init(void)
{
    AppData d = {0};
    g_assert_cmpint(atomic_load(&d.audio_recovery.fail_count), ==, 0);
    g_assert_false(atomic_load(&d.audio_recovery.recovering));
}

static void
test_audio_fail_increments_on_failure(void)
{
    AppData d = {0};
    audio_record_failure(&d);
    g_assert_cmpint(atomic_load(&d.audio_recovery.fail_count), ==, 1);
    g_assert_true(atomic_load(&d.audio_recovery.recovering));
}

static void
test_audio_fail_caps_at_max(void)
{
    AppData d = {0};
    for (int i = 0; i < AUDIO_MAX_RESTARTS + 10; i++)
        audio_record_failure(&d);
    g_assert_cmpint(atomic_load(&d.audio_recovery.fail_count), ==, AUDIO_MAX_RESTARTS);
}

static void
test_audio_recovering_flag_set_after_failure(void)
{
    AppData d = {0};
    g_assert_false(atomic_load(&d.audio_recovery.recovering));
    audio_record_failure(&d);
    g_assert_true(atomic_load(&d.audio_recovery.recovering));
}

static void
test_audio_recovering_cleared_on_success(void)
{
    AppData d = {0};
    audio_record_failure(&d);
    audio_record_failure(&d);
    audio_record_success(&d);
    g_assert_cmpint(atomic_load(&d.audio_recovery.fail_count), ==, 0);
    g_assert_false(atomic_load(&d.audio_recovery.recovering));
}

static void
test_audio_backoff_delay_ms(void)
{
    g_assert_cmpint(audio_backoff_ms(0), ==, 500);
    g_assert_cmpint(audio_backoff_ms(1), ==, 1000);
    g_assert_cmpint(audio_backoff_ms(2), ==, 2000);
    g_assert_cmpint(audio_backoff_ms(3), ==, 4000);
    g_assert_cmpint(audio_backoff_ms(4), ==, 8000);
    /* Powers of 2 past the cap must all return 30000. */
    g_assert_cmpint(audio_backoff_ms(10), ==, 30000);
    g_assert_cmpint(audio_backoff_ms(100), ==, 30000);
}

static void
test_audio_should_retry_true_below_max(void)
{
    AppData d = {0};
    for (int i = 0; i < AUDIO_MAX_RESTARTS - 1; i++)
        audio_record_failure(&d);
    g_assert_true(audio_should_retry(&d));
}

static void
test_audio_should_retry_false_at_max(void)
{
    AppData d = {0};
    for (int i = 0; i < AUDIO_MAX_RESTARTS; i++)
        audio_record_failure(&d);
    g_assert_false(audio_should_retry(&d));
}

static void
test_audio_success_after_max_resets_retry(void)
{
    AppData d = {0};
    /* Exhaust the budget. */
    for (int i = 0; i < AUDIO_MAX_RESTARTS; i++)
        audio_record_failure(&d);
    g_assert_false(audio_should_retry(&d));

    /* A successful reconnect (e.g. user replaces device) clears the slate. */
    audio_record_success(&d);
    g_assert_true(audio_should_retry(&d));
    g_assert_cmpint(atomic_load(&d.audio_recovery.fail_count), ==, 0);
}

static void
test_audio_backoff_negative_returns_base(void)
{
    /* Negative count should not crash — treat as 0. */
    g_assert_cmpint(audio_backoff_ms(-1), ==, 500);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/audio-recovery/fail-count-zero-on-init",
                    test_audio_fail_count_zero_on_init);
    g_test_add_func("/audio-recovery/fail-increments-on-failure",
                    test_audio_fail_increments_on_failure);
    g_test_add_func("/audio-recovery/fail-caps-at-max",
                    test_audio_fail_caps_at_max);
    g_test_add_func("/audio-recovery/recovering-flag-set-after-failure",
                    test_audio_recovering_flag_set_after_failure);
    g_test_add_func("/audio-recovery/recovering-cleared-on-success",
                    test_audio_recovering_cleared_on_success);
    g_test_add_func("/audio-recovery/backoff-delay-ms",
                    test_audio_backoff_delay_ms);
    g_test_add_func("/audio-recovery/should-retry-true-below-max",
                    test_audio_should_retry_true_below_max);
    g_test_add_func("/audio-recovery/should-retry-false-at-max",
                    test_audio_should_retry_false_at_max);
    g_test_add_func("/audio-recovery/success-after-max-resets-retry",
                    test_audio_success_after_max_resets_retry);
    g_test_add_func("/audio-recovery/backoff-negative-returns-base",
                    test_audio_backoff_negative_returns_base);

    return g_test_run();
}
