#pragma once

#include "app.h"

/**
 * Record a PipeWire stream failure.
 * Increments audio_fail_count (capped at AUDIO_MAX_RESTARTS) and sets
 * audio_recovering. Safe to call from the PipeWire thread.
 */
void audio_record_failure(AppData *d);

/**
 * Record a successful PipeWire stream connection.
 * Resets audio_fail_count to 0 and clears audio_recovering.
 * Safe to call from the PipeWire thread.
 */
void audio_record_success(AppData *d);

/**
 * Return the reprobe delay in milliseconds for the given failure count.
 * Formula: 500 * 2^fail_count, capped at 30000 ms.
 */
int audio_backoff_ms(int fail_count);

/**
 * Return true if the recovery budget has not been exhausted
 * (audio_fail_count < AUDIO_MAX_RESTARTS).
 */
bool audio_should_retry(AppData *d);
