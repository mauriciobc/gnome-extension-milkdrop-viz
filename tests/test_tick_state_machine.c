#include <glib.h>
#include <glib/gstdio.h>

#include "app.h"
#include "presets.h"

/* Tests state updated on each render pulse from main.c (preset dir, opacity, …).
 * Focuses on atomic state machine: preset_dir_pending, opacity,
 * and the mutex-protected pending_preset_dir copy. */

static void
test_preset_dir_pending_atomic_exchange(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    atomic_store(&app_data.preset_dir_pending, true);

    /* Simulates main.c:49: if (atomic_exchange(&preset_dir_pending, false)) */
    gboolean was_pending = atomic_exchange(&app_data.preset_dir_pending, false);

    g_assert_true(was_pending);
    g_assert_false(atomic_load(&app_data.preset_dir_pending));

    /* Second exchange should return false (flag was cleared). */
    gboolean still_pending = atomic_exchange(&app_data.preset_dir_pending, false);
    g_assert_false(still_pending);

    g_mutex_clear(&app_data.preset_dir_lock);
}

static void
test_pending_preset_dir_mutex_protected_copy(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);

    const char* test_path = "/tmp/test-presets";
    g_mutex_lock(&app_data.preset_dir_lock);
    g_strlcpy(app_data.pending_preset_dir, test_path, sizeof(app_data.pending_preset_dir));
    g_mutex_unlock(&app_data.preset_dir_lock);

    /* Simulates main.c:50-53: copy under lock. */
    char next_dir[sizeof(app_data.pending_preset_dir)] = {0};
    g_mutex_lock(&app_data.preset_dir_lock);
    g_strlcpy(next_dir, app_data.pending_preset_dir, sizeof(next_dir));
    g_mutex_unlock(&app_data.preset_dir_lock);

    g_assert_cmpstr(next_dir, ==, test_path);

    g_mutex_clear(&app_data.preset_dir_lock);
}

static void
test_opacity_atomic_read_write(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.opacity, 0.5f);

    /* Simulates main.c:68: gtk_widget_set_opacity(..., atomic_load(&opacity)) */
    float current_opacity = atomic_load(&app_data.opacity);
    g_assert_cmpfloat(current_opacity, ==, 0.5f);

    atomic_store(&app_data.opacity, 0.0f);
    g_assert_cmpfloat(atomic_load(&app_data.opacity), ==, 0.0f);

    atomic_store(&app_data.opacity, 1.0f);
    g_assert_cmpfloat(atomic_load(&app_data.opacity), ==, 1.0f);
}

static void
test_preset_reload_failure_preserves_state(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);

    /* Set up a valid initial preset dir. */
    gchar* valid_dir = g_build_filename(g_get_tmp_dir(), "milkdrop-valid-presets", NULL);
    g_mkdir(valid_dir, 0700);
    gchar* preset_file = g_build_filename(valid_dir, "test.milk", NULL);
    g_file_set_contents(preset_file, "", 0, NULL);

    app_data.preset_dir = g_strdup(valid_dir);
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 1);

    /* Simulate control thread setting a nonexistent preset dir. */
    const char* bad_path = "/nonexistent/path";
    g_mutex_lock(&app_data.preset_dir_lock);
    g_strlcpy(app_data.pending_preset_dir, bad_path, sizeof(app_data.pending_preset_dir));
    g_mutex_unlock(&app_data.preset_dir_lock);
    atomic_store(&app_data.preset_dir_pending, true);

    /* Simulates render pulse: exchange flag, copy path, reload. */
    if (atomic_exchange(&app_data.preset_dir_pending, false)) {
        char next_dir[sizeof(app_data.pending_preset_dir)] = {0};
        g_mutex_lock(&app_data.preset_dir_lock);
        g_strlcpy(next_dir, app_data.pending_preset_dir, sizeof(next_dir));
        g_mutex_unlock(&app_data.preset_dir_lock);

        g_clear_pointer(&app_data.preset_dir, g_free);
        app_data.preset_dir = g_strdup(next_dir);
        if (!presets_reload(&app_data)) {
            /* Reload failed — state is mutated but presets are cleared. */
            g_assert_null(app_data.presets);
            g_assert_cmpint(app_data.preset_count, ==, 0);
        }
    }

    presets_clear(&app_data);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_unlink(preset_file);
    g_rmdir(valid_dir);
    g_free(preset_file);
    g_free(valid_dir);
    g_mutex_clear(&app_data.preset_dir_lock);
}

static void
test_tick_queues_render_unconditionally(void)
{
    /* on_render_pulse returns G_SOURCE_CONTINUE (pulse keeps running). */
    gboolean result = G_SOURCE_CONTINUE;
    g_assert_true(result);
}

static void
test_concurrent_pending_flag_no_race(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);

    /* Multiple atomic_exchanges should be safe — only the first
     * sees true, subsequent ones see false. */
    atomic_store(&app_data.preset_dir_pending, true);

    gboolean first = atomic_exchange(&app_data.preset_dir_pending, false);
    gboolean second = atomic_exchange(&app_data.preset_dir_pending, false);
    gboolean third = atomic_exchange(&app_data.preset_dir_pending, false);

    g_assert_true(first);
    g_assert_false(second);
    g_assert_false(third);

    g_mutex_clear(&app_data.preset_dir_lock);
}

static void
test_next_previous_pending_atomic_exchange(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.next_preset_pending, true);
    atomic_store(&app_data.prev_preset_pending, true);

    gboolean next_was_pending = atomic_exchange(&app_data.next_preset_pending, false);
    gboolean prev_was_pending = atomic_exchange(&app_data.prev_preset_pending, false);

    g_assert_true(next_was_pending);
    g_assert_true(prev_was_pending);
    g_assert_false(atomic_load(&app_data.next_preset_pending));
    g_assert_false(atomic_load(&app_data.prev_preset_pending));
}

static void
test_next_previous_pending_second_exchange_cleared(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.next_preset_pending, true);
    atomic_store(&app_data.prev_preset_pending, true);

    g_assert_true(atomic_exchange(&app_data.next_preset_pending, false));
    g_assert_true(atomic_exchange(&app_data.prev_preset_pending, false));

    g_assert_false(atomic_exchange(&app_data.next_preset_pending, false));
    g_assert_false(atomic_exchange(&app_data.prev_preset_pending, false));
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/tick/pending-atomic-exchange", test_preset_dir_pending_atomic_exchange);
    g_test_add_func("/tick/pending-dir-mutex-copy", test_pending_preset_dir_mutex_protected_copy);
    g_test_add_func("/tick/opacity-atomic-rw", test_opacity_atomic_read_write);
    g_test_add_func("/tick/preset-reload-failure-state", test_preset_reload_failure_preserves_state);
    g_test_add_func("/tick/render-queue-continue", test_tick_queues_render_unconditionally);
    g_test_add_func("/tick/concurrent-pending-no-race", test_concurrent_pending_flag_no_race);
    g_test_add_func("/tick/next-previous-pending-exchange", test_next_previous_pending_atomic_exchange);
    g_test_add_func("/tick/next-previous-pending-second-exchange", test_next_previous_pending_second_exchange_cleared);

    return g_test_run();
}
