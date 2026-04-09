#include "presets.h"

#include <glib.h>

static gint
compare_paths(gconstpointer a, gconstpointer b)
{
    const char* lhs = *(const char* const*)a;
    const char* rhs = *(const char* const*)b;
    return g_strcmp0(lhs, rhs);
}

static bool
has_milk_extension(const char* name)
{
    if (!name)
        return false;

    gsize length = strlen(name);
    if (length < 5)
        return false;

    return g_ascii_strcasecmp(name + (length - 5), ".milk") == 0;
}

void
presets_clear(AppData* app_data)
{
    if (!app_data || !app_data->presets)
        return;

    g_strfreev(app_data->presets);
    app_data->presets = NULL;
    app_data->preset_count = 0;
    app_data->preset_current = 0;
}

static void
collect_presets(const char* dir_path, GPtrArray* entries)
{
    GDir* dir = g_dir_open(dir_path, 0, NULL);
    if (!dir)
        return;

    const char* name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '!')
            continue;

        gchar* full_path = g_build_filename(dir_path, name, NULL);

        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            collect_presets(full_path, entries);
            g_free(full_path);
        } else if (has_milk_extension(name)) {
            g_ptr_array_add(entries, full_path);
        } else {
            g_free(full_path);
        }
    }
    g_dir_close(dir);
}

bool
presets_reload(AppData* app_data)
{
    if (!app_data)
        return false;

    presets_clear(app_data);

    if (!app_data->preset_dir || app_data->preset_dir[0] == '\0')
        return true;

    if (!g_file_test(app_data->preset_dir, G_FILE_TEST_IS_DIR))
        return false;

    GPtrArray* entries = g_ptr_array_new_with_free_func(g_free);
    collect_presets(app_data->preset_dir, entries);

    g_ptr_array_sort(entries, compare_paths);
    g_ptr_array_add(entries, NULL);

    app_data->presets = (char**)g_ptr_array_free(entries, FALSE);
    app_data->preset_count = g_strv_length(app_data->presets);
    app_data->preset_current = 0;
    return true;
}

const char*
presets_current(const AppData* app_data)
{
    if (!app_data || !app_data->presets || app_data->preset_count <= 0)
        return NULL;

    if (app_data->preset_current < 0 || app_data->preset_current >= app_data->preset_count)
        return NULL;

    return app_data->presets[app_data->preset_current];
}
