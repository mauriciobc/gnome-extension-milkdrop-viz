#include "quarantine.h"

#include <stdatomic.h>
#include <string.h>

#include <glib.h>

void
quarantine_add(AppData *d, const char *path)
{
    if (!d || !path || path[0] == '\0')
        return;

    int count = atomic_load(&d->quarantine_count);

    /* Ignore duplicates. */
    for (int i = 0; i < count; i++) {
        if (strcmp(d->quarantine_list[i], path) == 0)
            return;
    }

    /* Silently cap — never overflow the fixed array. */
    if (count >= MAX_QUARANTINE)
        return;

    g_strlcpy(d->quarantine_list[count], path, MILKDROP_PATH_MAX);
    atomic_store(&d->quarantine_count, count + 1);
}

bool
quarantine_is_quarantined(const AppData *d, const char *path)
{
    if (!d || !path)
        return false;

    int count = atomic_load(&d->quarantine_count);
    for (int i = 0; i < count; i++) {
        if (strcmp(d->quarantine_list[i], path) == 0)
            return true;
    }
    return false;
}

void
quarantine_record_failure(AppData *d, const char *path)
{
    if (!d)
        return;

    quarantine_add(d, path);
    d->consecutive_failures++;

    if (d->consecutive_failures >= QUARANTINE_FAILURE_THRESHOLD)
        atomic_store(&d->quarantine_all_failed, true);
}

void
quarantine_record_success(AppData *d, const char *path)
{
    if (!d)
        return;

    d->consecutive_failures = 0;
    atomic_store(&d->quarantine_all_failed, false);

    if (path && path[0] != '\0') {
        g_mutex_lock(&d->last_preset_lock);
        g_strlcpy(d->last_good_preset, path, MILKDROP_PATH_MAX);
        g_mutex_unlock(&d->last_preset_lock);
    }
}
