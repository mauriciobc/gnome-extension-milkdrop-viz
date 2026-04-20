#include <glib.h>
#include <glib/gstdio.h>

#include "presets.h"

static gchar*
make_temp_dir(void)
{
    gchar* path = g_strdup_printf("%s/milkdrop-presets-edge-%u", g_get_tmp_dir(), g_random_int());
    g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
    return path;
}

static void
test_presets_current_negative_index(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    g_autofree gchar* dir = make_temp_dir();
    g_autofree gchar* p1 = g_build_filename(dir, "alpha.milk", NULL);
    g_file_set_contents(p1, "", 0, NULL);

    app_data.preset_dir = g_strdup(dir);
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 1);

    /* Manually set negative index. */
    app_data.preset_current = -1;

    g_assert_null(presets_current(&app_data));

    presets_clear(&app_data);
    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_mutex_clear(&app_data.preset_dir_lock);
    g_unlink(p1);
    g_rmdir(dir);
}

static void
test_presets_current_out_of_range_index(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    g_autofree gchar* dir = make_temp_dir();
    g_autofree gchar* p1 = g_build_filename(dir, "alpha.milk", NULL);
    g_file_set_contents(p1, "", 0, NULL);

    app_data.preset_dir = g_strdup(dir);
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 1);

    /* Manually set out-of-range index. */
    app_data.preset_current = 999;

    g_assert_null(presets_current(&app_data));

    presets_clear(&app_data);
    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_mutex_clear(&app_data.preset_dir_lock);
    g_unlink(p1);
    g_rmdir(dir);
}

static void
test_presets_reload_empty_directory(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    g_autofree gchar* dir = make_temp_dir();

    /* Directory exists but has no .milk files. */
    g_autofree gchar* junk = g_build_filename(dir, "readme.txt", NULL);
    g_file_set_contents(junk, "not a preset", 12, NULL);

    app_data.preset_dir = g_strdup(dir);
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 0);
    /* presets is non-NULL but points to an empty string array (just NULL terminator). */
    g_assert_nonnull(app_data.presets);
    g_assert_null(app_data.presets[0]);
    g_assert_null(presets_current(&app_data));

    presets_clear(&app_data);
    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_mutex_clear(&app_data.preset_dir_lock);
    g_unlink(junk);
    g_rmdir(dir);
}

static void
test_presets_reload_overwrites_previous_array(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);

    /* First directory with 2 presets. */
    g_autofree gchar* dir1 = make_temp_dir();
    g_autofree gchar* p1a = g_build_filename(dir1, "aaa.milk", NULL);
    g_autofree gchar* p1b = g_build_filename(dir1, "bbb.milk", NULL);
    g_file_set_contents(p1a, "", 0, NULL);
    g_file_set_contents(p1b, "", 0, NULL);

    app_data.preset_dir = g_strdup(dir1);
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 2);

    /* Second directory with 1 preset. */
    g_autofree gchar* dir2 = make_temp_dir();
    g_autofree gchar* p2a = g_build_filename(dir2, "zzz.milk", NULL);
    g_file_set_contents(p2a, "", 0, NULL);

    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    app_data.preset_dir = g_strdup(dir2);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 1);
    g_assert_cmpstr(app_data.presets[0], ==, p2a);

    /* The old array pointer should have been freed — verify by
     * checking preset_current returns the new preset. */
    g_assert_cmpstr(presets_current(&app_data), ==, p2a);

    presets_clear(&app_data);
    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_mutex_clear(&app_data.preset_dir_lock);
    g_unlink(p1a);
    g_unlink(p1b);
    g_rmdir(dir1);
    g_unlink(p2a);
    g_rmdir(dir2);
}

static void
test_presets_reload_missing_directory_preserves_previous_array(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);

    g_autofree gchar* dir = make_temp_dir();
    g_autofree gchar* preset = g_build_filename(dir, "keep.milk", NULL);
    g_file_set_contents(preset, "", 0, NULL);

    app_data.preset_dir = g_strdup(dir);
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 1);
    g_assert_cmpstr(presets_current(&app_data), ==, preset);

    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    app_data.preset_dir = g_strdup("/tmp/milkdrop-no-such-presets-dir");
    g_mutex_unlock(&app_data.preset_dir_lock);

    g_assert_false(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 1);
    g_assert_cmpstr(presets_current(&app_data), ==, preset);

    presets_clear(&app_data);
    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_mutex_clear(&app_data.preset_dir_lock);
    g_unlink(preset);
    g_rmdir(dir);
}

static void
test_presets_null_app_data(void)
{
    g_assert_false(presets_reload(NULL));
    g_assert_null(presets_current(NULL));
    presets_clear(NULL);
}

static void
test_presets_empty_string_dir(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    app_data.preset_dir = g_strdup("");

    /* Empty string should be treated as "no presets" — returns true
     * but clears any existing preset array. */
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 0);

    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_mutex_clear(&app_data.preset_dir_lock);
}

static void
test_presets_reload_rejects_overlong_dir_without_clearing_previous_array(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);

    g_autofree gchar* dir = make_temp_dir();
    g_autofree gchar* preset = g_build_filename(dir, "keep.milk", NULL);
    g_assert_true(g_file_set_contents(preset, "", 0, NULL));

    app_data.preset_dir = g_strdup(dir);
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 1);
    g_assert_cmpstr(presets_current(&app_data), ==, preset);

    g_autofree gchar* overlong_dir = g_malloc0((gsize)MILKDROP_PATH_MAX + 8);
    memset(overlong_dir, 'x', (size_t)MILKDROP_PATH_MAX);
    overlong_dir[MILKDROP_PATH_MAX] = '\0';

    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    app_data.preset_dir = g_strdup(overlong_dir);
    g_mutex_unlock(&app_data.preset_dir_lock);

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*preset-dir path too long*");
    g_assert_false(presets_reload(&app_data));
    g_test_assert_expected_messages();
    g_assert_cmpint(app_data.preset_count, ==, 1);
    g_assert_cmpstr(presets_current(&app_data), ==, preset);

    presets_clear(&app_data);
    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_mutex_clear(&app_data.preset_dir_lock);
    g_unlink(preset);
    g_rmdir(dir);
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/presets-edge/current-negative-index", test_presets_current_negative_index);
    g_test_add_func("/presets-edge/current-out-of-range", test_presets_current_out_of_range_index);
    g_test_add_func("/presets-edge/empty-directory", test_presets_reload_empty_directory);
    g_test_add_func("/presets-edge/reload-overwrites-array", test_presets_reload_overwrites_previous_array);
    g_test_add_func("/presets-edge/reload-missing-dir-preserves-array", test_presets_reload_missing_directory_preserves_previous_array);
    g_test_add_func("/presets-edge/null-app-data", test_presets_null_app_data);
    g_test_add_func("/presets-edge/empty-string-dir", test_presets_empty_string_dir);
    g_test_add_func("/presets-edge/reload-overlong-dir-preserves-array", test_presets_reload_rejects_overlong_dir_without_clearing_previous_array);

    return g_test_run();
}
