/**
 * test_preset_quarantine.c
 *
 * Unit tests for the preset quarantine subsystem.
 * Tests operate directly on AppData and quarantine.c without spawning a
 * process or socket — pure state-machine coverage.
 */

#include <glib.h>
#include <string.h>

#include "app.h"
#include "quarantine.h"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static AppData *
make_app_data(void)
{
    AppData *d = g_new0(AppData, 1);
    return d;
}

static void
free_app_data(AppData *d)
{
    g_free(d);
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void
test_quarantine_list_empty_on_init(void)
{
    AppData *d = make_app_data();

    g_assert_cmpint(d->quarantine.count, ==, 0);
    g_assert_false(atomic_load(&d->quarantine.all_failed));
    g_assert_cmpint(d->quarantine.consecutive_failures, ==, 0);
    g_assert_cmpstr(d->quarantine.last_good_preset, ==, "");

    free_app_data(d);
}

static void
test_quarantine_add_single(void)
{
    AppData *d = make_app_data();

    quarantine_add(d, "/presets/bad.milk");

    g_assert_cmpint(d->quarantine.count, ==, 1);
    g_assert_cmpstr(d->quarantine.list[0], ==, "/presets/bad.milk");

    free_app_data(d);
}

static void
test_quarantine_add_duplicate_ignored(void)
{
    AppData *d = make_app_data();

    quarantine_add(d, "/presets/bad.milk");
    quarantine_add(d, "/presets/bad.milk");

    g_assert_cmpint(d->quarantine.count, ==, 1);

    free_app_data(d);
}

static void
test_quarantine_add_multiple_distinct(void)
{
    AppData *d = make_app_data();

    quarantine_add(d, "/presets/a.milk");
    quarantine_add(d, "/presets/b.milk");
    quarantine_add(d, "/presets/c.milk");

    g_assert_cmpint(d->quarantine.count, ==, 3);

    free_app_data(d);
}

static void
test_quarantine_full_list_does_not_overflow(void)
{
    AppData *d = make_app_data();
    char path[MILKDROP_PATH_MAX];

    for (int i = 0; i < MAX_QUARANTINE + 10; i++) {
        g_snprintf(path, sizeof(path), "/presets/bad_%04d.milk", i);
        quarantine_add(d, path);
    }

    g_assert_cmpint(d->quarantine.count, <=, MAX_QUARANTINE);

    free_app_data(d);
}

static void
test_quarantine_is_quarantined_false_unknown(void)
{
    AppData *d = make_app_data();

    g_assert_false(quarantine_is_quarantined(d, "/presets/unknown.milk"));

    free_app_data(d);
}

static void
test_quarantine_is_quarantined_true_known(void)
{
    AppData *d = make_app_data();

    quarantine_add(d, "/presets/known.milk");

    g_assert_true(quarantine_is_quarantined(d, "/presets/known.milk"));
    g_assert_false(quarantine_is_quarantined(d, "/presets/other.milk"));

    free_app_data(d);
}

static void
test_consecutive_failures_threshold(void)
{
    AppData *d = make_app_data();

    for (int i = 0; i < QUARANTINE_FAILURE_THRESHOLD; i++) {
        char path[MILKDROP_PATH_MAX];
        g_snprintf(path, sizeof(path), "/presets/bad_%d.milk", i);
        quarantine_record_failure(d, path);
        if (i < QUARANTINE_FAILURE_THRESHOLD - 1)
            g_assert_false(atomic_load(&d->quarantine.all_failed));
    }

    g_assert_true(atomic_load(&d->quarantine.all_failed));
    g_assert_cmpint(d->quarantine.consecutive_failures, ==, QUARANTINE_FAILURE_THRESHOLD);

    free_app_data(d);
}

static void
test_consecutive_failures_reset_on_success(void)
{
    AppData *d = make_app_data();

    quarantine_record_failure(d, "/presets/a.milk");
    quarantine_record_failure(d, "/presets/b.milk");

    g_assert_cmpint(d->quarantine.consecutive_failures, ==, 2);

    quarantine_record_success(d, "/presets/good.milk");

    g_assert_cmpint(d->quarantine.consecutive_failures, ==, 0);
    g_assert_false(atomic_load(&d->quarantine.all_failed));

    free_app_data(d);
}

static void
test_last_good_preset_updated_on_success(void)
{
    AppData *d = make_app_data();

    quarantine_record_success(d, "/presets/good.milk");

    g_assert_cmpstr(d->quarantine.last_good_preset, ==, "/presets/good.milk");

    /* Subsequent success updates the stored path. */
    quarantine_record_success(d, "/presets/better.milk");
    g_assert_cmpstr(d->quarantine.last_good_preset, ==, "/presets/better.milk");

    free_app_data(d);
}

static void
test_record_failure_adds_to_quarantine(void)
{
    AppData *d = make_app_data();

    quarantine_record_failure(d, "/presets/broken.milk");

    g_assert_true(quarantine_is_quarantined(d, "/presets/broken.milk"));

    free_app_data(d);
}

static void
test_quarantine_add_null_path_ignored(void)
{
    AppData *d = make_app_data();

    quarantine_add(d, NULL);
    quarantine_add(d, "");

    g_assert_cmpint(d->quarantine.count, ==, 0);

    free_app_data(d);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/quarantine/empty-on-init",
                    test_quarantine_list_empty_on_init);
    g_test_add_func("/quarantine/add-single",
                    test_quarantine_add_single);
    g_test_add_func("/quarantine/add-duplicate-ignored",
                    test_quarantine_add_duplicate_ignored);
    g_test_add_func("/quarantine/add-multiple-distinct",
                    test_quarantine_add_multiple_distinct);
    g_test_add_func("/quarantine/full-list-no-overflow",
                    test_quarantine_full_list_does_not_overflow);
    g_test_add_func("/quarantine/is-quarantined-false-unknown",
                    test_quarantine_is_quarantined_false_unknown);
    g_test_add_func("/quarantine/is-quarantined-true-known",
                    test_quarantine_is_quarantined_true_known);
    g_test_add_func("/quarantine/consecutive-failures-threshold",
                    test_consecutive_failures_threshold);
    g_test_add_func("/quarantine/consecutive-failures-reset-on-success",
                    test_consecutive_failures_reset_on_success);
    g_test_add_func("/quarantine/last-good-preset-updated",
                    test_last_good_preset_updated_on_success);
    g_test_add_func("/quarantine/record-failure-adds-to-quarantine",
                    test_record_failure_adds_to_quarantine);
    g_test_add_func("/quarantine/add-null-path-ignored",
                    test_quarantine_add_null_path_ignored);

    return g_test_run();
}
