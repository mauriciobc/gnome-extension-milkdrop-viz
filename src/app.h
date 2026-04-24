#pragma once

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gtk/gtk.h>

typedef struct _OffscreenRenderer OffscreenRenderer;

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
#include <projectM-4/playlist.h>
#else
typedef void* projectm_handle;
typedef void* projectm_playlist_handle;
#endif

#define MILKDROP_RING_CAPACITY 16384u

typedef struct {
    float samples[MILKDROP_RING_CAPACITY];
    atomic_uint write_index;
    atomic_uint read_index;
} AudioRing;

/* Transition / beat-detection settings (control thread writes, GL thread reads). */
typedef struct {
    _Atomic float  beat_sensitivity;       /* 0.0–5.0, default 1.0 */
    _Atomic bool   hard_cut_enabled;       /* default false */
    _Atomic float  hard_cut_sensitivity;   /* 0.0–5.0, default 2.0 */
    _Atomic double hard_cut_duration;      /* 1.0–120.0 s, default 20.0 */
    _Atomic double soft_cut_duration;      /* 1.0–30.0 s, default 3.0 */
    /* Sync tracking — GL-thread-only */
    float  last_synced_beat_sensitivity;
    bool   last_synced_hard_cut_enabled;
    float  last_synced_hard_cut_sensitivity;
    double last_synced_hard_cut_duration;
    double last_synced_soft_cut_duration;
} TransitionState;

/* Preset quarantine — mostly GL thread; quarantine_count and
 * last_good_preset are also read by the control thread (status/save-state). */
#define MAX_QUARANTINE 64
#define QUARANTINE_FAILURE_THRESHOLD 5

typedef struct {
    char   list[MAX_QUARANTINE][MILKDROP_PATH_MAX];
    _Atomic int  count;
    int  consecutive_failures;
    GMutex last_preset_lock;
    char   last_good_preset[MILKDROP_PATH_MAX];
    _Atomic bool all_failed;
} PresetQuarantine;

/* Screenshot request — control thread writes path+flag, GL thread reads. */
typedef struct {
    _Atomic bool requested;
    GMutex lock;
    char   path[MILKDROP_PATH_MAX];
} ScreenshotState;

/* Audio recovery state machine. */
#define AUDIO_MAX_RESTARTS 5

typedef struct {
    _Atomic int  fail_count;
    _Atomic bool recovering;
} AudioRecoveryState;

typedef struct {
    GtkApplication* app;
    GtkWindow* window;
    GtkPicture* picture;
    OffscreenRenderer* offscreen;
    /** g_timeout_add: ~60 Hz; GdkFrameClock tick para de disparar com janela "parada" atrás. */
    guint render_pulse_source_id;

    projectm_handle projectm;
    projectm_playlist_handle playlist;
    /** HAVE_PROJECTM: skip further init attempts after irrecoverable offscreen renderer failure. */
    _Atomic bool projectm_init_aborted;

    AudioRing ring;

    char* preset_dir;
    char* textures_dir;
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
    int initial_width;
    int initial_height;

    int control_fd;
    GThread* control_thread;
    _Atomic bool control_thread_running;

    void* pw_state;

    gboolean verbose;

    GMutex preset_dir_lock;
    char pending_preset_dir[MILKDROP_PATH_MAX];

    /* Target frame rate set via control socket (written by control thread, read by GL thread). */
    _Atomic int fps_runtime;
    /* Last measured frame rate (written by GL thread, read by control thread). */
    _Atomic float fps_last;
    /* Preset rotation interval in seconds (written by control thread, read by GL thread). */
    _Atomic int preset_rotation_interval;

    TransitionState transitions;

    /* GL-thread-only fields for FPS tracking and timer rescheduling. */
    int    fps_applied;        /* last fps_runtime used for g_timeout interval; -1 until first pulse */
    gint64 fps_last_render_us; /* g_get_monotonic_time() of the previous rendered frame */

    /* Sync tracking to avoid per-frame API calls when values haven't changed.
     * All fields are GL-thread-only (no atomics needed). */
    float last_synced_opacity;
    bool  last_synced_shuffle;
    int   last_synced_interval;
    int   last_synced_render_width;
    int   last_synced_render_height;
    int   last_synced_projectm_fps;  /* last fps value sent to projectM via projectm_set_fps() */
    int   pulse_frame_count;  /* periodic pulse log counter */
    int   render_frame_counter; /* frames successfully rendered; guards projectm_load_preset_file warmup */

    /* Render-time PCM buffer — GL thread only, avoids per-frame 64 KB stack alloc. */
    float pcm_render_buf[MILKDROP_RING_CAPACITY];
    /* Cached result of projectm_pcm_get_max_samples(); 0 until projectM init. */
    unsigned int pcm_max_samples_per_channel;

    AudioRecoveryState audio_recovery;

    PresetQuarantine quarantine;

    /* restore-state: control thread writes path+paused under quarantine.last_preset_lock,
     * then sets the atomic flag; GL thread consumes on next render tick. */
    char         restore_preset_path[MILKDROP_PATH_MAX];
    bool         restore_preset_paused;
    _Atomic bool restore_state_pending;

    ScreenshotState screenshot;

    gint64 pm_mono_origin_us;
} AppData;

static inline void
audio_ring_init(AudioRing* ring)
{
    atomic_store(&ring->write_index, 0u);
    atomic_store(&ring->read_index, 0u);
}

#include <string.h>

static inline size_t
audio_ring_push(AudioRing* ring, const float* input, size_t count)
{
    if (!input || count == 0)
        return 0;

    unsigned int write_idx = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
    unsigned int read_idx = atomic_load_explicit(&ring->read_index, memory_order_acquire);

    unsigned int available;
    if (write_idx >= read_idx) {
        available = MILKDROP_RING_CAPACITY - write_idx + read_idx - 1u;
    } else {
        available = read_idx - write_idx - 1u;
    }

    size_t to_write = count > available ? available : count;
    if (to_write == 0)
        return 0;

    unsigned int first_part = MILKDROP_RING_CAPACITY - write_idx;
    if (to_write <= first_part) {
        memcpy(&ring->samples[write_idx], input, to_write * sizeof(float));
    } else {
        memcpy(&ring->samples[write_idx], input, first_part * sizeof(float));
        memcpy(&ring->samples[0], input + first_part, (to_write - first_part) * sizeof(float));
    }

    atomic_store_explicit(&ring->write_index, (write_idx + to_write) % MILKDROP_RING_CAPACITY, memory_order_release);
    return to_write;
}

static inline size_t
audio_ring_read(AudioRing* ring, float* output, size_t max_count)
{
    if (!output || max_count == 0)
        return 0;

    unsigned int read_idx = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
    unsigned int write_idx = atomic_load_explicit(&ring->write_index, memory_order_acquire);

    unsigned int available;
    if (write_idx >= read_idx) {
        available = write_idx - read_idx;
    } else {
        available = MILKDROP_RING_CAPACITY - read_idx + write_idx;
    }

    size_t to_read = max_count > available ? available : max_count;
    if (to_read == 0)
        return 0;

    unsigned int first_part = MILKDROP_RING_CAPACITY - read_idx;
    if (to_read <= first_part) {
        memcpy(output, &ring->samples[read_idx], to_read * sizeof(float));
    } else {
        memcpy(output, &ring->samples[read_idx], first_part * sizeof(float));
        memcpy(output + first_part, &ring->samples[0], (to_read - first_part) * sizeof(float));
    }

    atomic_store_explicit(&ring->read_index, (read_idx + to_read) % MILKDROP_RING_CAPACITY, memory_order_release);
    return to_read;
}
