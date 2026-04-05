#include "audio_recovery.h"

#define AUDIO_BACKOFF_BASE_MS 500
#define AUDIO_BACKOFF_MAX_MS  30000

void
audio_record_failure(AppData *d)
{
    if (!d)
        return;

    /* Cap at AUDIO_MAX_RESTARTS so the status response stays meaningful. */
    int current = atomic_load_explicit(&d->audio_fail_count, memory_order_relaxed);
    if (current < AUDIO_MAX_RESTARTS)
        atomic_store_explicit(&d->audio_fail_count, current + 1, memory_order_relaxed);

    atomic_store_explicit(&d->audio_recovering, true, memory_order_release);
}

void
audio_record_success(AppData *d)
{
    if (!d)
        return;

    atomic_store_explicit(&d->audio_fail_count, 0, memory_order_relaxed);
    atomic_store_explicit(&d->audio_recovering, false, memory_order_release);
}

int
audio_backoff_ms(int fail_count)
{
    if (fail_count <= 0)
        return AUDIO_BACKOFF_BASE_MS;

    long ms = AUDIO_BACKOFF_BASE_MS;
    for (int i = 0; i < fail_count; i++) {
        ms *= 2;
        if (ms >= AUDIO_BACKOFF_MAX_MS)
            return AUDIO_BACKOFF_MAX_MS;
    }
    return (int)ms;
}

bool
audio_should_retry(AppData *d)
{
    if (!d)
        return false;

    return atomic_load_explicit(&d->audio_fail_count, memory_order_relaxed) < AUDIO_MAX_RESTARTS;
}
