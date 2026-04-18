/*
 * Integration scan: same recursive .milk discovery as presets_start_async_scan / playlist fill.
 * Default path: cream-of-the-crop (large tree). Override with MILKDROP_TEST_PRESET_DIR.
 */
#include <glib.h>

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
    app_data.preset_dir = g_strdup(dir);

    gboolean ok = presets_reload(&app_data);
    g_assert_true(ok);
    /* cream-of-the-crop ships thousands of presets; adjust floor if pack shrinks */
    g_assert_cmpint(app_data.preset_count, >, 500);

    g_message("scanned %d presets under %s", app_data.preset_count, dir);

    presets_clear(&app_data);
    g_clear_pointer(&app_data.preset_dir, g_free);
}

int
main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/presets/cream-tree-scan", test_scan_full_preset_tree_matches_installed_pack);
    return g_test_run();
}
