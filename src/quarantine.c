#include "quarantine.h"

#include <string.h>

#include <glib.h>

void
quarantine_add(AppData *d, const char *path)
{
    if (!d || !path || path[0] == '\0')
        return;

    /* Ignore duplicates. */
    for (int i = 0; i < d->quarantine_count; i++) {
        if (strcmp(d->quarantine_list[i], path) == 0)
            return;
    }

    /* Silently cap — never overflow the fixed array. */
    if (d->quarantine_count >= MAX_QUARANTINE)
        return;

    g_strlcpy(d->quarantine_list[d->quarantine_count], path, MILKDROP_PATH_MAX);
    d->quarantine_count++;
}

bool
quarantine_is_quarantined(const AppData *d, const char *path)
{
    if (!d || !path)
        return false;

    for (int i = 0; i < d->quarantine_count; i++) {
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

    if (path && path[0] != '\0')
        g_strlcpy(d->last_good_preset, path, MILKDROP_PATH_MAX);
}
