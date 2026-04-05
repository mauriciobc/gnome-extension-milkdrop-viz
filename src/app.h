#pragma once

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gtk/gtk.h>

/* Max bytes for preset-dir control paths (incl. trailing NUL in buffers). */
#ifdef PATH_MAX
#define MILKDROP_PATH_MAX PATH_MAX
#else
#define MILKDROP_PATH_MAX 4096
#endif

#ifndef HAVE_PROJECTM
#define HAVE_PROJECTM 0
#endif

#if HAVE_PROJECTM
#include <projectM-4/projectM.h>
#else
typedef void* projectm_handle;
#endif

#define MILKDROP_RING_CAPACITY 16384u

typedef struct {
    float samples[MILKDROP_RING_CAPACITY];
    atomic_uint write_index;
    atomic_uint read_index;
} AudioRing;

typedef struct {
    GtkApplication* app;
    GtkWindow* window;
    GtkGLArea* gl_area;
    /** g_timeout_add: ~60 Hz; GdkFrameClock tick para de disparar com janela “parada” atrás. */
    guint render_pulse_source_id;

    projectm_handle projectm;
    void* projectm_playlist;
    /** HAVE_PROJECTM: skip further init attempts until next GtkGLArea realize after irrecoverable failure. */
    _Atomic bool projectm_init_aborted;

    AudioRing ring;

    char* preset_dir;
    char** presets;
    int preset_count;
    int preset_current;
    char* socket_path;
    int monitor_index;
    bool shuffle;

    _Atomic bool pause_audio;
    _Atomic bool shutdown_requested;
    _Atomic float opacity;
    _Atomic bool overlay_enabled;
    _Atomic bool shuffle_runtime;
    _Atomic bool preset_dir_pending;
    _Atomic bool next_preset_pending;
    _Atomic bool prev_preset_pending;

    int render_width;
    int render_height;

    int control_fd;
    GThread* control_thread;
    _Atomic bool control_thread_running;

    void* pw_state;

    gboolean verbose;

    _Atomic bool idle_surface_queued;

    GMutex preset_dir_lock;
    char pending_preset_dir[MILKDROP_PATH_MAX];

    /* Target frame rate set via control socket (written by control thread, read by GL thread). */
    _Atomic int fps_runtime;
    /* Last measured frame rate (written by GL thread, read by control thread). */
    _Atomic float fps_last;
    /* GL-thread-only fields for FPS tracking and timer rescheduling. */
    int    fps_applied;        /* last fps_runtime value used to set the timer */
    gint64 fps_last_pulse_us;  /* g_get_monotonic_time() of the previous render pulse */

    /* Audio recovery state machine. */
#define AUDIO_MAX_RESTARTS 5
    _Atomic int  audio_fail_count;
    _Atomic bool audio_recovering;

    /* Preset quarantine — GL thread only (no atomics needed). */
#define MAX_QUARANTINE 64
#define QUARANTINE_FAILURE_THRESHOLD 5
    char quarantine_list[MAX_QUARANTINE][MILKDROP_PATH_MAX];
    int  quarantine_count;
    int  consecutive_failures;
    char last_good_preset[MILKDROP_PATH_MAX];
    _Atomic bool quarantine_all_failed;
} AppData;

static inline void
audio_ring_init(AudioRing* ring)
{
    atomic_store(&ring->write_index, 0u);
    atomic_store(&ring->read_index, 0u);
}

static inline size_t
audio_ring_push(AudioRing* ring, const float* input, size_t count)
{
    if (!input || count == 0)
        return 0;

    unsigned int write_idx = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
    unsigned int read_idx = atomic_load_explicit(&ring->read_index, memory_order_acquire);
    size_t written = 0;

    while (written < count) {
        unsigned int next_write = (write_idx + 1u) % MILKDROP_RING_CAPACITY;
        if (next_write == read_idx)
            break;

        ring->samples[write_idx] = input[written];
        write_idx = next_write;
        written++;
    }

    atomic_store_explicit(&ring->write_index, write_idx, memory_order_release);
    return written;
}

static inline size_t
audio_ring_read(AudioRing* ring, float* output, size_t max_count)
{
    if (!output || max_count == 0)
        return 0;

    unsigned int read_idx = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
    unsigned int write_idx = atomic_load_explicit(&ring->write_index, memory_order_acquire);
    size_t copied = 0;

    while (copied < max_count && read_idx != write_idx) {
        output[copied] = ring->samples[read_idx];
        read_idx = (read_idx + 1u) % MILKDROP_RING_CAPACITY;
        copied++;
    }

    atomic_store_explicit(&ring->read_index, read_idx, memory_order_release);
    return copied;
}