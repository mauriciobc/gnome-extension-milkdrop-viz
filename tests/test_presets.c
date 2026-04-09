#include <glib.h>
#include <glib/gstdio.h>

#include "presets.h"

static gchar*
make_temp_dir(void)
{
    gchar* path = g_strdup_printf("%s/milkdrop-presets-%u", g_get_tmp_dir(), g_random_int());
    g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
    return path;
}

static void
test_presets_reload_filters_and_sorts(void)
{
    AppData app_data = {0};
    g_autofree gchar* dir = make_temp_dir();
    g_autofree gchar* p1 = g_build_filename(dir, "zeta.milk", NULL);
    g_autofree gchar* p2 = g_build_filename(dir, "alpha.milk", NULL);
    g_autofree gchar* p3 = g_build_filename(dir, "ignore.txt", NULL);

    g_assert_true(g_file_set_contents(p1, "", 0, NULL));
    g_assert_true(g_file_set_contents(p2, "", 0, NULL));
    g_assert_true(g_file_set_contents(p3, "", 0, NULL));

    app_data.preset_dir = g_strdup(dir);
    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, ==, 2);
    g_assert_cmpstr(app_data.presets[0], ==, p2);
    g_assert_cmpstr(app_data.presets[1], ==, p1);
    g_assert_cmpstr(presets_current(&app_data), ==, p2);

    presets_clear(&app_data);
    g_clear_pointer(&app_data.preset_dir, g_free);

    g_unlink(p1);
    g_unlink(p2);
    g_unlink(p3);
    g_rmdir(dir);
}

static void
test_presets_reload_scans_subdirectories(void)
{
    AppData app_data = {0};
    g_autofree gchar* dir = make_temp_dir();
    g_autofree gchar* nested = g_build_filename(dir, "pack", "set-a", NULL);
    g_autofree gchar* top_level = g_build_filename(dir, "root.milk", NULL);
    g_autofree gchar* nested_milk = g_build_filename(nested, "deep.MILK", NULL);
    g_autofree gchar* nested_txt = g_build_filename(nested, "ignore.txt", NULL);

    g_assert_cmpint(g_mkdir_with_parents(nested, 0700), ==, 0);
    g_assert_true(g_file_set_contents(top_level, "", 0, NULL));
    g_assert_true(g_file_set_contents(nested_milk, "", 0, NULL));
    g_assert_true(g_file_set_contents(nested_txt, "", 0, NULL));

    app_data.preset_dir = g_strdup(dir);
    g_assert_true(presets_reload(&app_data));

    g_assert_cmpint(app_data.preset_count, ==, 2);
    g_assert_true(g_strv_contains((const gchar* const*)app_data.presets, top_level));
    g_assert_true(g_strv_contains((const gchar* const*)app_data.presets, nested_milk));

    presets_clear(&app_data);
    g_clear_pointer(&app_data.preset_dir, g_free);

    g_unlink(top_level);
    g_unlink(nested_milk);
    g_unlink(nested_txt);
    g_rmdir(nested);
    g_autofree gchar* nested_parent = g_build_filename(dir, "pack", NULL);
    g_rmdir(nested_parent);
    g_rmdir(dir);
}

static void
test_presets_reload_handles_missing_directory(void)
{
    AppData app_data = {0};
    app_data.preset_dir = g_strdup("/tmp/milkdrop-no-such-presets-dir");

    g_assert_false(presets_reload(&app_data));
    g_assert_null(app_data.presets);
    g_assert_cmpint(app_data.preset_count, ==, 0);

    g_clear_pointer(&app_data.preset_dir, g_free);
}

static void
test_presets_reload_ignores_folders_starting_with_bang(void)
{
    AppData app_data = {0};
    g_autofree gchar* dir = make_temp_dir();
    g_autofree gchar* valid_preset = g_build_filename(dir, "valid.milk", NULL);
    g_autofree gchar* bang_dir = g_build_filename(dir, "!Transitions", NULL);
    g_autofree gchar* bang_preset = g_build_filename(bang_dir, "fade.milk", NULL);

    g_assert_cmpint(g_mkdir(bang_dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(valid_preset, "", 0, NULL));
    g_assert_true(g_file_set_contents(bang_preset, "", 0, NULL));

    app_data.preset_dir = g_strdup(dir);
    g_assert_true(presets_reload(&app_data));

    g_assert_cmpint(app_data.preset_count, ==, 1);
    g_assert_true(g_strv_contains((const gchar* const*)app_data.presets, valid_preset));
    g_assert_false(g_strv_contains((const gchar* const*)app_data.presets, bang_preset));

    presets_clear(&app_data);
    g_clear_pointer(&app_data.preset_dir, g_free);

    g_unlink(valid_preset);
    g_unlink(bang_preset);
    g_rmdir(bang_dir);
    g_rmdir(dir);
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/presets/reload-filters-and-sorts", test_presets_reload_filters_and_sorts);
    g_test_add_func("/presets/reload-scans-subdirectories", test_presets_reload_scans_subdirectories);
    g_test_add_func("/presets/reload-missing-dir", test_presets_reload_handles_missing_directory);
    g_test_add_func("/presets/reload-ignores-bang-folders", test_presets_reload_ignores_folders_starting_with_bang);

    return g_test_run();
}
