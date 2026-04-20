/*
 * Integration scan: same recursive .milk discovery as presets_start_async_scan / playlist fill.
 * Default path: cream-of-the-crop (large tree). Override with MILKDROP_TEST_PRESET_DIR.
 */
#include <glib.h>
#include <string.h>

#include "app.h"
#include "presets.h"

#ifndef MILKDROP_DEFAULT_CREAM_DIR
#define MILKDROP_DEFAULT_CREAM_DIR "/usr/local/share/projectM/presets/cream-of-the-crop"
#endif

static void
test_scan_full_preset_tree_matches_installed_pack(void)
{
    const char* dir = g_getenv("MILKDROP_TEST_PRESET_DIR");
    if (!dir || dir[0] == '\0')
        dir = MILKDROP_DEFAULT_CREAM_DIR;

    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        g_test_skip_printf("preset dir not found: %s (set MILKDROP_TEST_PRESET_DIR)", dir);
        return;
    }

    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    app_data.preset_dir = g_strdup(dir);

    gboolean ok = presets_reload(&app_data);
    g_assert_true(ok);
    /* cream-of-the-crop ships thousands of presets; adjust floor if pack shrinks */
    g_assert_cmpint(app_data.preset_count, >, 500);

    g_message("scanned %d presets under %s", app_data.preset_count, dir);

    presets_clear(&app_data);
    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_mutex_clear(&app_data.preset_dir_lock);
}

static void
test_scan_preserves_long_paths_and_filenames(void)
{
    const char* dir = g_getenv("MILKDROP_TEST_PRESET_DIR");
    if (!dir || dir[0] == '\0')
        dir = MILKDROP_DEFAULT_CREAM_DIR;

    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        g_test_skip_printf("preset dir not found: %s (set MILKDROP_TEST_PRESET_DIR)", dir);
        return;
    }

    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    app_data.preset_dir = g_strdup(dir);

    g_assert_true(presets_reload(&app_data));
    g_assert_cmpint(app_data.preset_count, >, 0);

    const char* longest_path = NULL;
    const char* longest_name_path = NULL;
    gsize max_path_len = 0;
    gsize max_name_len = 0;

    for (int i = 0; i < app_data.preset_count; i++) {
        const char* preset_path = app_data.presets[i];
        g_assert_nonnull(preset_path);

        gsize path_len = strlen(preset_path);
        g_assert_cmpuint(path_len, <, MILKDROP_PATH_MAX);
        if (path_len > max_path_len) {
            max_path_len = path_len;
            longest_path = preset_path;
        }

        const char* base = g_path_get_basename(preset_path);
        gsize name_len = strlen(base);
        g_assert_cmpuint(name_len, <, MILKDROP_PATH_MAX);
        if (name_len > max_name_len) {
            max_name_len = name_len;
            longest_name_path = preset_path;
        }
        g_free((gpointer)base);
    }

    g_assert_nonnull(longest_path);
    g_assert_nonnull(longest_name_path);
    g_assert_true(g_strv_contains((const gchar* const*)app_data.presets, longest_path));
    g_assert_true(g_strv_contains((const gchar* const*)app_data.presets, longest_name_path));

    g_message("longest preset path: %" G_GSIZE_FORMAT " bytes :: %s", max_path_len, longest_path);
    g_message("longest preset name: %" G_GSIZE_FORMAT " bytes :: %s", max_name_len, longest_name_path);

    presets_clear(&app_data);
    g_mutex_lock(&app_data.preset_dir_lock);
    g_clear_pointer(&app_data.preset_dir, g_free);
    g_mutex_unlock(&app_data.preset_dir_lock);
    g_mutex_clear(&app_data.preset_dir_lock);
}

int
main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/presets/cream-tree-scan", test_scan_full_preset_tree_matches_installed_pack);
    g_test_add_func("/presets/cream-long-paths", test_scan_preserves_long_paths_and_filenames);
    return g_test_run();
}
