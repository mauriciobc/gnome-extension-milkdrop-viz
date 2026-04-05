#pragma once

#include "app.h"

/**
 * Add a preset path to the quarantine list.
 * Ignores NULL/empty paths and duplicate entries.
 * Silently caps at MAX_QUARANTINE.
 */
void quarantine_add(AppData *d, const char *path);

/**
 * Return true if path is in the quarantine list.
 */
bool quarantine_is_quarantined(const AppData *d, const char *path);

/**
 * Record a preset switch failure.
 * Adds path to the quarantine list, increments consecutive_failures,
 * and sets quarantine_all_failed when QUARANTINE_FAILURE_THRESHOLD is reached.
 */
void quarantine_record_failure(AppData *d, const char *path);

/**
 * Record a successful preset switch.
 * Resets consecutive_failures and quarantine_all_failed.
 * Stores path in last_good_preset (may be NULL/empty to skip update).
 */
void quarantine_record_success(AppData *d, const char *path);
