#include "audio.h"
#include "audio_recovery.h"

size_t
audio_align_stereo_float_count(size_t sample_count)
{
    return sample_count - (sample_count % 2u);
}

#if HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

#if HAVE_PIPEWIRE
typedef struct {
    struct pw_thread_loop* loop;
    struct pw_context* context;
    struct pw_core* core;
    struct pw_stream* stream;
    struct spa_hook stream_listener;
    AppData* app_data;
} PipeWireState;

static void
on_stream_process(void* userdata)
{
    PipeWireState* state = userdata;
    if (!state || !state->stream || !state->app_data)
        return;

    struct pw_buffer* pw_buffer = pw_stream_dequeue_buffer(state->stream);
    if (!pw_buffer)
        return;

    struct spa_buffer* spa_buffer = pw_buffer->buffer;
    if (!spa_buffer || spa_buffer->n_datas < 1 || !spa_buffer->datas[0].data) {
        pw_stream_queue_buffer(state->stream, pw_buffer);
        return;
    }

    const struct spa_chunk* chunk = spa_buffer->datas[0].chunk;
    if (!chunk || chunk->size == 0) {
        pw_stream_queue_buffer(state->stream, pw_buffer);
        return;
    }

    const uint8_t* bytes = SPA_PTROFF(spa_buffer->datas[0].data, chunk->offset, const uint8_t);
    size_t sample_count = chunk->size / sizeof(float);

    sample_count = audio_align_stereo_float_count(sample_count);
    if (sample_count > 0)
        (void)audio_ring_push(&state->app_data->ring, (const float*)bytes, sample_count);

    pw_stream_queue_buffer(state->stream, pw_buffer);
}

static gboolean
audio_reprobe_cb(gpointer user_data)
{
    AppData *d = user_data;
    if (!d || atomic_load(&d->shutdown_requested))
        return G_SOURCE_REMOVE;

    g_message("milkdrop: audio reprobe attempt %d/%d",
              atomic_load(&d->audio_recovery.fail_count), AUDIO_MAX_RESTARTS);

    /* Full teardown then reinit — runs on the GLib main loop, not PipeWire thread. */
    audio_cleanup(d);
    audio_init(d);
    return G_SOURCE_REMOVE;
}

static void
on_stream_state_changed(void              *userdata,
                        enum pw_stream_state old_state,
                        enum pw_stream_state new_state,
                        const char         *error)
{
    PipeWireState *pw = userdata;
    if (!pw || !pw->app_data)
        return;

    AppData *d = pw->app_data;

    switch (new_state) {
    case PW_STREAM_STATE_ERROR:
        g_warning("milkdrop: PipeWire stream error: %s", error ? error : "(unknown)");
        /* fall through — treat error as an unrecoverable disconnect */
        /* fall through */
    case PW_STREAM_STATE_UNCONNECTED:
        /* Ignore the initial UNCONNECTED state before any connection attempt. */
        if (new_state == PW_STREAM_STATE_UNCONNECTED &&
            old_state != PW_STREAM_STATE_PAUSED &&
            old_state != PW_STREAM_STATE_STREAMING &&
            old_state != PW_STREAM_STATE_ERROR)
            break;

        /* Only schedule one reprobe at a time. */
        if (atomic_load(&d->audio_recovery.recovering))
            break;

        audio_record_failure(d);

        if (audio_should_retry(d)) {
            int delay_ms = audio_backoff_ms(atomic_load(&d->audio_recovery.fail_count));
            g_timeout_add(delay_ms, audio_reprobe_cb, d);
        } else {
            g_warning("milkdrop: audio recovery budget exhausted after %d attempts",
                      AUDIO_MAX_RESTARTS);
        }
        break;

    case PW_STREAM_STATE_STREAMING:
        audio_record_success(d);
        break;

    default:
        break;
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process       = on_stream_process,
    .state_changed = on_stream_state_changed,
};

static gboolean
audio_init_pipewire(AppData* app_data)
{
    PipeWireState* state = g_new0(PipeWireState, 1);
    if (!state)
        return FALSE;

    state->app_data = app_data;
    app_data->pw_state = state;

    pw_init(NULL, NULL);

    state->loop = pw_thread_loop_new("milkdrop-pipewire", NULL);
    if (!state->loop)
        goto fail;

    if (pw_thread_loop_start(state->loop) != 0)
        goto fail;

    pw_thread_loop_lock(state->loop);

    state->context = pw_context_new(pw_thread_loop_get_loop(state->loop), NULL, 0);
    if (!state->context)
        goto fail_locked;

    state->core = pw_context_connect(state->context, NULL, 0);
    if (!state->core)
        goto fail_locked;

    struct pw_properties* properties = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Music",
      PW_KEY_STREAM_CAPTURE_SINK, "true",
      NULL);

    state->stream = pw_stream_new(state->core, "milkdrop-audio", properties);
    if (!state->stream)
        goto fail_locked;

    pw_stream_add_listener(state->stream, &state->stream_listener, &stream_events, state);

    struct spa_audio_info_raw audio_info = {
        .format = SPA_AUDIO_FORMAT_F32_LE,
        .rate = 48000,
        .channels = 2,
    };
    audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
    audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;

    uint8_t buffer[1024] = {0};
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[1] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audio_info),
    };

    if (pw_stream_connect(state->stream,
                          PW_DIRECTION_INPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
                          params,
                          1)
        != 0)
        goto fail_locked;

    pw_thread_loop_unlock(state->loop);
    if (app_data->verbose)
        g_message("PipeWire audio capture initialized");
    return TRUE;

fail_locked:
    pw_thread_loop_unlock(state->loop);
fail:
    g_warning("PipeWire initialization failed; running with silent audio input");
    audio_cleanup(app_data);
    return FALSE;
}
#endif

bool
audio_init(AppData* app_data)
{
    if (!app_data)
        return false;

    audio_ring_init(&app_data->ring);

#if HAVE_PIPEWIRE
    if (!audio_init_pipewire(app_data))
        return true; /* Non-fatal: continue with silent audio input. */
#else
    if (app_data->verbose)
        g_message("PipeWire support not built; running with silent audio input");
#endif

    return true;
}

void
audio_cleanup(AppData* app_data)
{
    if (!app_data)
        return;

#if HAVE_PIPEWIRE
    PipeWireState* state = app_data->pw_state;
    if (!state)
        return;

    if (state->loop)
        pw_thread_loop_lock(state->loop);

    if (state->stream) {
        pw_stream_disconnect(state->stream);
        pw_stream_destroy(state->stream);
        state->stream = NULL;
    }

    if (state->core) {
        pw_core_disconnect(state->core);
        state->core = NULL;
    }

    if (state->context) {
        pw_context_destroy(state->context);
        state->context = NULL;
    }

    if (state->loop) {
        pw_thread_loop_unlock(state->loop);
        pw_thread_loop_stop(state->loop);
        pw_thread_loop_destroy(state->loop);
        state->loop = NULL;
    }

    g_free(state);
    app_data->pw_state = NULL;
#else
    (void)app_data;
#endif
}