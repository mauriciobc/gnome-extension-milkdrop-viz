/*
 * Invariant for src/main.c on_render async preset batches:
 * only the first batch that introduces playlist entries may call
 * milkdrop_reset_startup_gate(app, true). Later batches append presets;
 * re-running the gate forces projectm_playlist_set_position(0) every few frames
 * (preset_switched spam, black screen on large dirs like cream-of-the-crop).
 *
 * Keep this predicate aligned with the condition in main.c.
 */
#include <glib.h>

static gboolean
predicate_async_chunk_should_reset_startup_gate(gboolean playlist_nonempty,
                                                  gboolean startup_deferred,
                                                  gboolean startup_final,
                                                  gboolean async_first_chunk_done)
{
    return playlist_nonempty && !startup_deferred && startup_final && !async_first_chunk_done;
}

static void
test_first_idle_to_nonempty_triggers_gate(void)
{
    g_assert_true(predicate_async_chunk_should_reset_startup_gate(TRUE, FALSE, TRUE, FALSE));
}

static void
test_while_deferred_activation_pending_no_second_gate(void)
{
    /* After first chunk, deferred=TRUE until activate_initial runs */
    g_assert_false(predicate_async_chunk_should_reset_startup_gate(TRUE, TRUE, FALSE, TRUE));
}

static void
test_after_initial_activation_extra_batches_do_not_gate(void)
{
    /* Bug case: deferred cleared, final=TRUE again, same preset index 0 re-forced */
    g_assert_false(predicate_async_chunk_should_reset_startup_gate(TRUE, FALSE, TRUE, TRUE));
}

int
main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/startup-async-gate/first-chunk", test_first_idle_to_nonempty_triggers_gate);
    g_test_add_func("/startup-async-gate/deferred-blocks", test_while_deferred_activation_pending_no_second_gate);
    g_test_add_func("/startup-async-gate/no-repeat-after-activation", test_after_initial_activation_extra_batches_do_not_gate);

    return g_test_run();
}
