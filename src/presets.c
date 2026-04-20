#include "presets.h"

#include <glib.h>
#include <string.h>

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

static bool
scan_should_stop(const AppData* app_data)
{
    return app_data && atomic_load(&app_data->preset_scan_stop);
}

void
presets_clear(AppData* app_data)
{
    if (!app_data)
        return;

    if (app_data->presets)
        g_strfreev(app_data->presets);
    app_data->presets = NULL;
    app_data->preset_count = 0;
    app_data->preset_current = 0;
}

bool
presets_apply_dir_change(AppData* app_data, const char* next_dir)
{
    if (!app_data || !next_dir)
        return false;

    if (next_dir[0] != '\0' && !g_file_test(next_dir, G_FILE_TEST_IS_DIR))
        return false;

    g_mutex_lock(&app_data->preset_dir_lock);
    g_clear_pointer(&app_data->preset_dir, g_free);
    app_data->preset_dir = g_strdup(next_dir);
    g_mutex_unlock(&app_data->preset_dir_lock);
    return true;
}

static void
collect_presets(const char*   dir_path,
                GPtrArray*    entries,
                int           depth,
                const AppData* app_data)
{
    if (scan_should_stop(app_data))
        return;

    if (depth > 32) {
        g_warning("presets: max recursion depth exceeded at %s", dir_path);
        return;
    }

    GDir* dir = g_dir_open(dir_path, 0, NULL);
    if (!dir)
        return;

    const char* name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (scan_should_stop(app_data))
            break;

        if (name[0] == '!')
            continue;

        gchar* full_path = g_build_filename(dir_path, name, NULL);

        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            collect_presets(full_path, entries, depth + 1, app_data);
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

    char dir_buf[MILKDROP_PATH_MAX];
    dir_buf[0] = '\0';
    g_mutex_lock(&app_data->preset_dir_lock);
    if (app_data->preset_dir)
        g_strlcpy(dir_buf, app_data->preset_dir, sizeof(dir_buf));
    g_mutex_unlock(&app_data->preset_dir_lock);

    if (dir_buf[0] == '\0')
    {
        presets_clear(app_data);
        return true;
    }

    if (!g_file_test(dir_buf, G_FILE_TEST_IS_DIR))
        return false;

    GPtrArray* entries = g_ptr_array_new_with_free_func(g_free);
    collect_presets(dir_buf, entries, 0, NULL);

    g_ptr_array_sort(entries, compare_paths);
    g_ptr_array_add(entries, NULL);

    char** new_presets = (char**)g_ptr_array_free(entries, FALSE);
    int    new_count = g_strv_length(new_presets);

    presets_clear(app_data);
    app_data->presets = new_presets;
    app_data->preset_count = new_count;
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

static void
clear_preset_load_queue_locked(AppData* app_data)
{
    if (app_data->preset_load_queue)
        g_ptr_array_set_size(app_data->preset_load_queue, 0);
}

/*
 * Executes one full scan attempt for the current preset_dir.
 * Returns true if the scan produced a valid, non-stale result that was
 * committed to the queue and pending_preset_flush was set.
 * Returns false if the result was discarded because seq changed mid-scan
 * (caller should retry).
 */
static bool
scan_once(AppData* app_data)
{
    if (scan_should_stop(app_data))
        return true;

    uint32_t seq_snapshot = atomic_load(&app_data->preset_scan_seq);

    char scan_dir[MILKDROP_PATH_MAX];
    scan_dir[0] = '\0';
    g_mutex_lock(&app_data->preset_dir_lock);
    if (app_data->preset_dir)
        g_strlcpy(scan_dir, app_data->preset_dir, sizeof(scan_dir));
    g_mutex_unlock(&app_data->preset_dir_lock);

    if (scan_should_stop(app_data))
        return true;

    if (scan_dir[0] == '\0' || !g_file_test(scan_dir, G_FILE_TEST_IS_DIR)) {
        /* No valid directory — only treat as done if seq is still current. */
        return scan_should_stop(app_data) ||
               atomic_load(&app_data->preset_scan_seq) == seq_snapshot;
    }

    GPtrArray* entries = g_ptr_array_new_with_free_func(g_free);
    collect_presets(scan_dir, entries, 0, app_data);

    if (scan_should_stop(app_data)) {
        g_ptr_array_free(entries, TRUE);
        return true;
    }

    g_ptr_array_sort(entries, compare_paths);

    if (scan_should_stop(app_data)) {
        g_ptr_array_free(entries, TRUE);
        return true;
    }

    if (atomic_load(&app_data->preset_scan_seq) != seq_snapshot) {
        g_ptr_array_free(entries, TRUE);
        return false;
    }

    g_mutex_lock(&app_data->preset_queue_lock);
    if (!app_data->preset_load_queue)
        app_data->preset_load_queue = g_ptr_array_new_with_free_func(g_free);
    for (int i = (int)entries->len - 1; i >= 0; i--)
        g_ptr_array_add(app_data->preset_load_queue, g_steal_pointer(&entries->pdata[i]));
    g_mutex_unlock(&app_data->preset_queue_lock);

    g_ptr_array_free(entries, TRUE);

    if (scan_should_stop(app_data)) {
        g_mutex_lock(&app_data->preset_queue_lock);
        clear_preset_load_queue_locked(app_data);
        g_mutex_unlock(&app_data->preset_queue_lock);
        return true;
    }

    /* Final seq check: only signal the GL thread if results are still fresh. */
    if (atomic_load(&app_data->preset_scan_seq) != seq_snapshot) {
        g_mutex_lock(&app_data->preset_queue_lock);
        clear_preset_load_queue_locked(app_data);
        g_mutex_unlock(&app_data->preset_queue_lock);
        return false;
    }

    atomic_store(&app_data->pending_preset_flush, true);
    return true;
}

static gpointer
presets_scan_thread_func(gpointer user_data)
{
    AppData* app_data = user_data;

    for (;;) {
        guint watchdog = 0;
        while (!scan_once(app_data)) {
            if (scan_should_stop(app_data))
                break;

            if (++watchdog > 64) {
                g_warning("presets: async scan exceeded iteration limit — giving up");
                break;
            }
        }

        uint32_t settled_seq = atomic_load(&app_data->preset_scan_seq);
        atomic_store(&app_data->is_scanning_presets, false);

        if (scan_should_stop(app_data))
            break;

        if (!atomic_exchange(&app_data->rescan_requested, false) &&
            atomic_load(&app_data->preset_scan_seq) == settled_seq)
            break;

        bool expected = false;
        if (!atomic_compare_exchange_strong(&app_data->is_scanning_presets, &expected, true))
            break;
    }

    return NULL;
}

void
presets_start_async_scan(AppData* app_data)
{
    if (!app_data)
        return;

    atomic_fetch_add(&app_data->preset_scan_seq, 1);

    if (atomic_exchange(&app_data->is_scanning_presets, true)) {
        atomic_store(&app_data->rescan_requested, true);
        return;
    }

    atomic_store(&app_data->preset_scan_stop, false);
    atomic_store(&app_data->rescan_requested, false);

    if (app_data->preset_scan_thread) {
        g_thread_join(app_data->preset_scan_thread);
        app_data->preset_scan_thread = NULL;
    }

    g_mutex_lock(&app_data->preset_queue_lock);
    clear_preset_load_queue_locked(app_data);
    g_mutex_unlock(&app_data->preset_queue_lock);

    atomic_store(&app_data->pending_preset_flush, false);

    GThread* thread = g_thread_try_new("preset-scanner", presets_scan_thread_func, app_data, NULL);
    if (thread) {
        app_data->preset_scan_thread = thread;
    } else {
        atomic_store(&app_data->is_scanning_presets, false);
    }
}

void
presets_cleanup_async(AppData* app_data)
{
    if (!app_data)
        return;

    atomic_store(&app_data->preset_scan_stop, true);
    atomic_store(&app_data->rescan_requested, false);

    if (app_data->preset_scan_thread) {
        g_thread_join(app_data->preset_scan_thread);
        app_data->preset_scan_thread = NULL;
    }

    atomic_store(&app_data->is_scanning_presets, false);
    atomic_store(&app_data->pending_preset_flush, false);

    g_mutex_lock(&app_data->preset_queue_lock);
    if (app_data->preset_load_queue) {
        g_ptr_array_free(app_data->preset_load_queue, TRUE);
        app_data->preset_load_queue = NULL;
    }
    g_mutex_unlock(&app_data->preset_queue_lock);
}

