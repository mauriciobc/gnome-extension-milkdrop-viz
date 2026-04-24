#include "control.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Escape characters that would break a JSON string literal.
 * Handles: backslash, double-quote, and control characters. */
static void
control_json_escape(const char *in, char *out, size_t out_size)
{
    if (out_size == 0) return;

    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_size; i++) {
        if (in[i] == '"' || in[i] == '\\') {
            out[j++] = '\\';
        } else if ((unsigned char)in[i] < 0x20) {
            /* Skip non-printable control characters. */
            continue;
        }
        out[j++] = in[i];
    }
    out[j] = '\0';
}

static bool
parse_on_off(const char* value, bool* out_enabled)
{
    if (!value || !out_enabled)
        return false;

    if (g_strcmp0(value, "on") == 0) {
        *out_enabled = true;
        return true;
    }

    if (g_strcmp0(value, "off") == 0) {
        *out_enabled = false;
        return true;
    }

    return false;
}

static bool
parse_double_range(const char* value, double min, double max, double* out_parsed)
{
    if (!value || !out_parsed)
        return false;

    char* endptr = NULL;
    errno = 0;
    double parsed = g_ascii_strtod(value, &endptr);
    if (errno != 0 || !endptr || *endptr != '\0' || parsed < min || parsed > max)
        return false;

    *out_parsed = parsed;
    return true;
}

static bool
parse_long_range(const char* value, long min, long max, long* out_parsed)
{
    if (!value || !out_parsed)
        return false;

    char* endptr = NULL;
    errno = 0;
    long parsed = strtol(value, &endptr, 10);
    if (errno != 0 || !endptr || *endptr != '\0' || parsed < min || parsed > max)
        return false;

    *out_parsed = parsed;
    return true;
}

/* Convenience: write an "ok …" response into the output buffer. */
#define RESPOND_OK(msg) do { g_strlcpy(response, msg, response_size); } while (0)

static gboolean
control_apply_command(AppData* app_data, const ControlCommand* command, gchar* response, gsize response_size)
{
    switch (command->type) {
    case CONTROL_CMD_STATUS: {
        int audio_fails = atomic_load(&app_data->audio_recovery.fail_count);
        const char *audio_status = (audio_fails == 0)                   ? "ok"
                                   : (audio_fails < AUDIO_MAX_RESTARTS) ? "recovering"
                                                                         : "failed";
        /* Snapshot last_good_preset under lock — it is written by the GL thread. */
        char preset_snapshot[MILKDROP_PATH_MAX];
        g_mutex_lock(&app_data->quarantine.last_preset_lock);
        g_strlcpy(preset_snapshot, app_data->quarantine.last_good_preset, sizeof(preset_snapshot));
        g_mutex_unlock(&app_data->quarantine.last_preset_lock);

        g_snprintf(response,
                   response_size,
                   "paused=%d\nopacity=%.3f\nshuffle=%d\noverlay=%d\nquarantine=%d\naudio=%s\nfps=%.1f\npreset=%s\n\n",
                   atomic_load(&app_data->pause_audio) ? 1 : 0,
                   atomic_load(&app_data->opacity),
                   atomic_load(&app_data->shuffle_runtime) ? 1 : 0,
                   atomic_load(&app_data->overlay_enabled) ? 1 : 0,
                    atomic_load(&app_data->quarantine.count),
                   audio_status,
                   atomic_load(&app_data->fps_last),
                   preset_snapshot);
        return TRUE;
    }
    case CONTROL_CMD_OPACITY:
        atomic_store(&app_data->opacity, command->opacity);                     RESPOND_OK("ok opacity\n");      return TRUE;
    case CONTROL_CMD_PAUSE:
        atomic_store(&app_data->pause_audio, command->pause_enabled);            RESPOND_OK("ok pause\n");        return TRUE;
    case CONTROL_CMD_SHUFFLE:
        atomic_store(&app_data->shuffle_runtime, command->bool_value);           RESPOND_OK("ok shuffle\n");      return TRUE;
    case CONTROL_CMD_OVERLAY:
        atomic_store(&app_data->overlay_enabled, command->bool_value);           RESPOND_OK("ok overlay\n");      return TRUE;
    case CONTROL_CMD_PRESET_DIR:
        g_mutex_lock(&app_data->preset_dir_lock);
        g_strlcpy(app_data->pending_preset_dir, command->text_value, sizeof(app_data->pending_preset_dir));
        g_mutex_unlock(&app_data->preset_dir_lock);
        atomic_store(&app_data->preset_dir_pending, true);                       RESPOND_OK("ok preset-dir\n");  return TRUE;
    case CONTROL_CMD_NEXT_PRESET:
        atomic_store(&app_data->next_preset_pending, true);                      RESPOND_OK("ok next\n");         return TRUE;
    case CONTROL_CMD_PREV_PRESET:
        atomic_store(&app_data->prev_preset_pending, true);                      RESPOND_OK("ok previous\n");    return TRUE;
    case CONTROL_CMD_FPS:
        atomic_store(&app_data->fps_runtime, command->int_value);                RESPOND_OK("ok fps\n");          return TRUE;
    case CONTROL_CMD_ROTATION_INTERVAL:
        atomic_store(&app_data->preset_rotation_interval, CLAMP(command->int_value, 5, 300));
                                                                                 RESPOND_OK("ok rotation-interval\n"); return TRUE;
    case CONTROL_CMD_BEAT_SENSITIVITY:
        atomic_store(&app_data->transitions.beat_sensitivity, command->float_value);         RESPOND_OK("ok beat-sensitivity\n");     return TRUE;
    case CONTROL_CMD_HARD_CUT_ENABLED:
        atomic_store(&app_data->transitions.hard_cut_enabled, command->bool_value);          RESPOND_OK("ok hard-cut-enabled\n");     return TRUE;
    case CONTROL_CMD_HARD_CUT_SENSITIVITY:
        atomic_store(&app_data->transitions.hard_cut_sensitivity, command->float_value);     RESPOND_OK("ok hard-cut-sensitivity\n"); return TRUE;
    case CONTROL_CMD_HARD_CUT_DURATION:
        atomic_store(&app_data->transitions.hard_cut_duration, command->double_value);       RESPOND_OK("ok hard-cut-duration\n");    return TRUE;
    case CONTROL_CMD_SOFT_CUT_DURATION:
        atomic_store(&app_data->transitions.soft_cut_duration, command->double_value);       RESPOND_OK("ok soft-cut-duration\n");    return TRUE;
    case CONTROL_CMD_SAVE_STATE: {
        /* Snapshot and JSON-escape last_good_preset under lock. */
        char preset_snapshot[MILKDROP_PATH_MAX];
        char escaped_preset[MILKDROP_PATH_MAX * 2];
        g_mutex_lock(&app_data->quarantine.last_preset_lock);
        g_strlcpy(preset_snapshot, app_data->quarantine.last_good_preset, sizeof(preset_snapshot));
        g_mutex_unlock(&app_data->quarantine.last_preset_lock);
        control_json_escape(preset_snapshot[0] != '\0' ? preset_snapshot : "",
                            escaped_preset, sizeof(escaped_preset));
        g_snprintf(response,
                   response_size,
                   "{\"preset\":\"%s\",\"paused\":%d,\"opacity\":%.3f,\"shuffle\":%d}\n",
                   escaped_preset,
                   atomic_load(&app_data->pause_audio) ? 1 : 0,
                   atomic_load(&app_data->opacity),
                   atomic_load(&app_data->shuffle_runtime) ? 1 : 0);
        return TRUE;
    }
    case CONTROL_CMD_RESTORE_STATE:
        if (command->text_value[0] != '\0') {
            g_mutex_lock(&app_data->quarantine.last_preset_lock);
            g_strlcpy(app_data->restore_preset_path, command->text_value,
                      sizeof(app_data->restore_preset_path));
            app_data->restore_preset_paused = command->int_value != 0;
            g_mutex_unlock(&app_data->quarantine.last_preset_lock);
            atomic_store(&app_data->restore_state_pending, true);
        }
        RESPOND_OK("ok restore-state\n"); return TRUE;
    case CONTROL_CMD_SCREENSHOT:
        /* Write path under lock; GL thread copies it out before use. */
        g_mutex_lock(&app_data->screenshot.lock);
        g_strlcpy(app_data->screenshot.path, command->screenshot_path,
                  sizeof(app_data->screenshot.path));
        g_mutex_unlock(&app_data->screenshot.lock);
        atomic_store(&app_data->screenshot.requested, true);
        RESPOND_OK("ok screenshot queued\n"); return TRUE;
    case CONTROL_CMD_NONE:
    default:
        RESPOND_OK("err invalid\n"); return FALSE;
    }
}

static void
control_handle_client(AppData* app_data, int client_fd)
{
    char buffer[MILKDROP_PATH_MAX + 128] = {0};
    size_t used = 0;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (used < sizeof(buffer) - 1) {
        ssize_t n = recv(client_fd, buffer + used, sizeof(buffer) - 1 - used, 0);
        if (n <= 0)
            break;
            
        char *newline = memchr(buffer + used, '\n', (size_t)n);
        used += (size_t)n;

        if (newline) {
            used = (size_t)(newline - buffer) + 1;
            break;
        }
    }

    if (used == 0)
        return;

    buffer[used] = '\0';

    ControlCommand command = {0};
    ControlParseResult parse_result = control_parse_command(buffer, &command);
    gchar response[MILKDROP_PATH_MAX * 2 + 256] = {0};

    if (parse_result == CONTROL_PARSE_OK)
        (void)control_apply_command(app_data, &command, response, sizeof(response));
    else if (parse_result == CONTROL_PARSE_INCOMPLETE)
        g_strlcpy(response, "err incomplete\n", sizeof(response));
    else if (parse_result == CONTROL_PARSE_PATH_TOO_LONG)
        g_strlcpy(response, "err path-too-long\n", sizeof(response));
    else
        g_strlcpy(response, "err invalid\n", sizeof(response));

    (void)send(client_fd, response, strlen(response), 0);
}

static gpointer
control_thread_main(gpointer data)
{
    AppData* app_data = data;
    struct pollfd pfd = {
        .fd = app_data->control_fd,
        .events = POLLIN,
    };

    while (atomic_load(&app_data->control_thread_running)) {
        int ready = poll(&pfd, 1, 200);
        if (ready <= 0)
            continue;

        if (pfd.revents & POLLIN) {
            int client_fd = accept(app_data->control_fd, NULL, NULL);
            if (client_fd < 0)
                continue;

            control_handle_client(app_data, client_fd);
            close(client_fd);
        }
    }

    return NULL;
}

ControlParseResult
control_parse_command(const char* line, ControlCommand* out_command)
{
    if (!line || !out_command)
        return CONTROL_PARSE_INVALID;

    *out_command = (ControlCommand){0};

    if (!g_str_has_suffix(line, "\n"))
        return CONTROL_PARSE_INCOMPLETE;

    g_autofree gchar* normalized = g_strdup(line);
    g_strchomp(normalized);

    gint argc = 0;
    g_auto(GStrv) argv = NULL;
    if (!g_shell_parse_argv(normalized, &argc, &argv, NULL) || argc == 0)
        return CONTROL_PARSE_INVALID;

    if (g_strcmp0(argv[0], "status") == 0 && argc == 1) {
        out_command->type = CONTROL_CMD_STATUS;
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "opacity") == 0 && argc == 2) {
        double parsed = 0.0;
        if (!parse_double_range(argv[1], 0.0, 1.0, &parsed))
            return CONTROL_PARSE_INVALID;
        out_command->type = CONTROL_CMD_OPACITY;
        out_command->opacity = (float)parsed;
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "pause") == 0 && argc == 2) {
        bool enabled = false;
        if (parse_on_off(argv[1], &enabled)) {
            out_command->type = CONTROL_CMD_PAUSE;
            out_command->pause_enabled = enabled;
            return CONTROL_PARSE_OK;
        }
        return CONTROL_PARSE_INVALID;
    }

#define PARSE_ON_OFF(name, cmd, field)                                         \
    if (g_strcmp0(argv[0], name) == 0 && argc == 2) {                          \
        bool enabled = false;                                                  \
        if (!parse_on_off(argv[1], &enabled))                                  \
            return CONTROL_PARSE_INVALID;                                      \
        out_command->type = cmd;                                               \
        out_command->field = enabled;                                          \
        return CONTROL_PARSE_OK;                                               \
    }

    PARSE_ON_OFF("shuffle",           CONTROL_CMD_SHUFFLE,           bool_value);
    PARSE_ON_OFF("overlay",           CONTROL_CMD_OVERLAY,           bool_value);
    PARSE_ON_OFF("hard-cut-enabled",  CONTROL_CMD_HARD_CUT_ENABLED,  bool_value);

#undef PARSE_ON_OFF

    if (g_strcmp0(argv[0], "preset-dir") == 0 && argc == 2) {
        if (argv[1][0] == '\0')
            return CONTROL_PARSE_INVALID;

        if (strlen(argv[1]) >= sizeof(out_command->text_value))
            return CONTROL_PARSE_PATH_TOO_LONG;

        out_command->type = CONTROL_CMD_PRESET_DIR;
        g_strlcpy(out_command->text_value, argv[1], sizeof(out_command->text_value));
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "next") == 0 && argc == 1) {
        out_command->type = CONTROL_CMD_NEXT_PRESET;
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "previous") == 0 && argc == 1) {
        out_command->type = CONTROL_CMD_PREV_PRESET;
        return CONTROL_PARSE_OK;
    }

#define PARSE_LONG(name, cmd, field, min, max)                                 \
    if (g_strcmp0(argv[0], name) == 0 && argc == 2) {                          \
        long parsed = 0;                                                       \
        if (!parse_long_range(argv[1], min, max, &parsed))                     \
            return CONTROL_PARSE_INVALID;                                      \
        out_command->type = cmd;                                               \
        out_command->field = (int)parsed;                                      \
        return CONTROL_PARSE_OK;                                               \
    }

    PARSE_LONG("fps",               CONTROL_CMD_FPS,               int_value, 10, 144);
    PARSE_LONG("rotation-interval", CONTROL_CMD_ROTATION_INTERVAL, int_value,  5, 300);

#undef PARSE_LONG

#define PARSE_DOUBLE(name, cmd, field, min, max, cast)                         \
    if (g_strcmp0(argv[0], name) == 0 && argc == 2) {                          \
        double parsed = 0.0;                                                   \
        if (!parse_double_range(argv[1], min, max, &parsed))                   \
            return CONTROL_PARSE_INVALID;                                      \
        out_command->type = cmd;                                               \
        out_command->field = (cast)parsed;                                     \
        return CONTROL_PARSE_OK;                                               \
    }

    PARSE_DOUBLE("beat-sensitivity",     CONTROL_CMD_BEAT_SENSITIVITY,     float_value,  0.0,   5.0, float);
    PARSE_DOUBLE("hard-cut-sensitivity", CONTROL_CMD_HARD_CUT_SENSITIVITY, float_value,  0.0,   5.0, float);
    PARSE_DOUBLE("hard-cut-duration",    CONTROL_CMD_HARD_CUT_DURATION,    double_value, 1.0, 120.0, double);
    PARSE_DOUBLE("soft-cut-duration",    CONTROL_CMD_SOFT_CUT_DURATION,    double_value, 1.0,  30.0, double);

#undef PARSE_DOUBLE

    if (g_strcmp0(argv[0], "save-state") == 0 && argc == 1) {
        out_command->type = CONTROL_CMD_SAVE_STATE;
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "restore-state") == 0 && argc >= 1) {
        out_command->type = CONTROL_CMD_RESTORE_STATE;
        if (argc >= 2) {
            if (strlen(argv[1]) >= sizeof(out_command->text_value))
                return CONTROL_PARSE_PATH_TOO_LONG;
            g_strlcpy(out_command->text_value, argv[1], sizeof(out_command->text_value));
        }
        if (argc >= 3) {
            long flag = 0;
            if (parse_long_range(argv[2], 0, 1, &flag))
                out_command->int_value = (int)flag;
        }
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "screenshot") == 0 && argc == 2) {
        if (strlen(argv[1]) >= sizeof(out_command->screenshot_path))
            return CONTROL_PARSE_PATH_TOO_LONG;

        out_command->type = CONTROL_CMD_SCREENSHOT;
        g_strlcpy(out_command->screenshot_path, argv[1], sizeof(out_command->screenshot_path));
        return CONTROL_PARSE_OK;
    }

    return CONTROL_PARSE_INVALID;
}

bool
status_response_parse(const char* response, StatusResponse* out)
{
    if (!response || !out)
        return false;

    *out = (StatusResponse){0};

    g_auto(GStrv) lines = g_strsplit(response, "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
        const char* line = lines[i];
        if (line[0] == '\0')
            break;

        const char* eq = strchr(line, '=');
        if (!eq)
            continue;

        char key[64] = {0};
        size_t keylen = (size_t)(eq - line);
        if (keylen >= sizeof(key))
            continue;
        memcpy(key, line, keylen);

        const char* value = eq + 1;

        if (strcmp(key, "fps") == 0) {
            out->fps = (float)g_ascii_strtod(value, NULL);
        } else if (strcmp(key, "paused") == 0) {
            out->paused = value[0] == '1';
        } else if (strcmp(key, "preset") == 0) {
            g_strlcpy(out->preset, value, sizeof(out->preset));
        } else if (strcmp(key, "audio") == 0) {
            if (strcmp(value, "recovering") == 0)
                out->audio = CONTROL_AUDIO_RECOVERING;
            else if (strcmp(value, "failed") == 0)
                out->audio = CONTROL_AUDIO_FAILED;
            else
                out->audio = CONTROL_AUDIO_OK;
        } else if (strcmp(key, "quarantine") == 0) {
            out->quarantine_count = (int)g_ascii_strtoll(value, NULL, 10);
        }
    }

    return true;
}

gchar*
control_socket_path_for_monitor(int monitor_index)
{
    const char* runtime_dir = g_get_user_runtime_dir();
    const char* base = runtime_dir ? runtime_dir : "/tmp";

    /* Meson runs C test binaries in parallel; default paths would collide on milkdrop-0.sock. */
    if (g_getenv("MILKDROP_TEST_ISOLATE_SOCKET") != NULL)
        return g_strdup_printf("%s/milkdrop-%d-%d.sock", base, monitor_index, (int)getpid());

    if (!runtime_dir)
        return g_strdup_printf("/tmp/milkdrop-%d.sock", monitor_index);
    return g_strdup_printf("%s/milkdrop-%d.sock", runtime_dir, monitor_index);
}

bool
control_init(AppData* app_data)
{
    if (!app_data)
        return false;

    if (!app_data->socket_path)
        app_data->socket_path = control_socket_path_for_monitor(app_data->monitor_index);

    g_unlink(app_data->socket_path);

    app_data->control_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (app_data->control_fd < 0) {
        g_warning("Failed to create control socket: %s", g_strerror(errno));
        return false;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    
    if (strlen(app_data->socket_path) >= sizeof(addr.sun_path)) {
        g_warning("Failed to bind control socket: path %s exceeds maximum length", app_data->socket_path);
        close(app_data->control_fd);
        app_data->control_fd = -1;
        return false;
    }
    
    g_strlcpy(addr.sun_path, app_data->socket_path, sizeof(addr.sun_path));

    if (bind(app_data->control_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        g_warning("Failed to bind control socket %s: %s", app_data->socket_path, g_strerror(errno));
        close(app_data->control_fd);
        app_data->control_fd = -1;
        return false;
    }

    if (listen(app_data->control_fd, 8) != 0) {
        g_warning("Failed to listen on control socket %s: %s", app_data->socket_path, g_strerror(errno));
        close(app_data->control_fd);
        app_data->control_fd = -1;
        g_unlink(app_data->socket_path);
        return false;
    }

    if (g_chmod(app_data->socket_path, 0600) != 0)
        g_warning("Failed to chmod control socket %s: %s", app_data->socket_path, g_strerror(errno));

    atomic_store(&app_data->control_thread_running, true);
    app_data->control_thread = g_thread_new("milkdrop-control", control_thread_main, app_data);
    if (!app_data->control_thread) {
        g_warning("Failed to start control thread");
        atomic_store(&app_data->control_thread_running, false);
        close(app_data->control_fd);
        app_data->control_fd = -1;
        g_unlink(app_data->socket_path);
        return false;
    }

    g_message("Control socket initialized at %s", app_data->socket_path);
    return true;
}

void
control_cleanup(AppData* app_data)
{
    if (!app_data)
        return;

    atomic_store(&app_data->control_thread_running, false);

    if (app_data->control_fd >= 0) {
        shutdown(app_data->control_fd, SHUT_RDWR);
        close(app_data->control_fd);
        app_data->control_fd = -1;
    }

    if (app_data->control_thread) {
        g_thread_join(app_data->control_thread);
        app_data->control_thread = NULL;
    }

    if (!app_data->socket_path)
        return;

    g_unlink(app_data->socket_path);
}