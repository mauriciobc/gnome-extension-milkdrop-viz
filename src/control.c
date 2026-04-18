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

static gboolean
control_apply_command(AppData* app_data, const ControlCommand* command, gchar* response, gsize response_size)
{
    switch (command->type) {
    case CONTROL_CMD_STATUS: {
        int audio_fails = atomic_load(&app_data->audio_fail_count);
        const char *audio_status = (audio_fails == 0)                   ? "ok"
                                   : (audio_fails < AUDIO_MAX_RESTARTS) ? "recovering"
                                                                         : "failed";
        /* Snapshot last_good_preset under lock — it is written by the GL thread. */
        char preset_snapshot[MILKDROP_PATH_MAX];
        g_mutex_lock(&app_data->last_preset_lock);
        g_strlcpy(preset_snapshot, app_data->last_good_preset, sizeof(preset_snapshot));
        g_mutex_unlock(&app_data->last_preset_lock);

        g_snprintf(response,
                   response_size,
                   "paused=%d\nopacity=%.3f\nshuffle=%d\noverlay=%d\nquarantine=%d\naudio=%s\nfps=%.1f\npreset=%s\n\n",
                   atomic_load(&app_data->pause_audio) ? 1 : 0,
                   atomic_load(&app_data->opacity),
                   atomic_load(&app_data->shuffle_runtime) ? 1 : 0,
                   atomic_load(&app_data->overlay_enabled) ? 1 : 0,
                   atomic_load(&app_data->quarantine_count),
                   audio_status,
                   atomic_load(&app_data->fps_last),
                   preset_snapshot);
        return TRUE;
    }
    case CONTROL_CMD_OPACITY:
        atomic_store(&app_data->opacity, command->opacity);
        g_strlcpy(response, "ok opacity\n", response_size);
        return TRUE;
    case CONTROL_CMD_PAUSE:
        atomic_store(&app_data->pause_audio, command->pause_enabled);
        g_strlcpy(response, "ok pause\n", response_size);
        return TRUE;
    case CONTROL_CMD_SHUFFLE:
        atomic_store(&app_data->shuffle_runtime, command->bool_value);
        g_strlcpy(response, "ok shuffle\n", response_size);
        return TRUE;
    case CONTROL_CMD_OVERLAY:
        atomic_store(&app_data->overlay_enabled, command->bool_value);
        g_strlcpy(response, "ok overlay\n", response_size);
        return TRUE;
    case CONTROL_CMD_PRESET_DIR:
        g_mutex_lock(&app_data->preset_dir_lock);
        g_strlcpy(app_data->pending_preset_dir, command->text_value, sizeof(app_data->pending_preset_dir));
        g_mutex_unlock(&app_data->preset_dir_lock);
        atomic_store(&app_data->preset_dir_pending, true);
        g_strlcpy(response, "ok preset-dir\n", response_size);
        return TRUE;
    case CONTROL_CMD_NEXT_PRESET:
        atomic_store(&app_data->next_preset_pending, true);
        g_strlcpy(response, "ok next\n", response_size);
        return TRUE;
    case CONTROL_CMD_PREV_PRESET:
        atomic_store(&app_data->prev_preset_pending, true);
        g_strlcpy(response, "ok previous\n", response_size);
        return TRUE;
    case CONTROL_CMD_FPS:
        atomic_store(&app_data->fps_runtime, command->int_value);
        g_strlcpy(response, "ok fps\n", response_size);
        return TRUE;
    case CONTROL_CMD_ROTATION_INTERVAL:
        atomic_store(&app_data->preset_rotation_interval, CLAMP(command->int_value, 5, 300));
        g_strlcpy(response, "ok rotation-interval\n", response_size);
        return TRUE;
    case CONTROL_CMD_SAVE_STATE: {
        /* Snapshot and JSON-escape last_good_preset under lock. */
        char preset_snapshot[MILKDROP_PATH_MAX];
        char escaped_preset[MILKDROP_PATH_MAX * 2];
        g_mutex_lock(&app_data->last_preset_lock);
        g_strlcpy(preset_snapshot, app_data->last_good_preset, sizeof(preset_snapshot));
        g_mutex_unlock(&app_data->last_preset_lock);
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
        // TODO: Implement full state restore
        g_strlcpy(response, "ok restore (partial)\n", response_size);
        return TRUE;
    case CONTROL_CMD_SCREENSHOT:
        /* Write path under lock; GL thread copies it out before use. */
        g_mutex_lock(&app_data->screenshot_lock);
        g_strlcpy(app_data->screenshot_path, command->screenshot_path,
                  sizeof(app_data->screenshot_path));
        g_mutex_unlock(&app_data->screenshot_lock);
        atomic_store(&app_data->screenshot_requested, true);
        g_strlcpy(response, "ok screenshot queued\n", response_size);
        return TRUE;
    case CONTROL_CMD_NONE:
    default:
        g_strlcpy(response, "err invalid\n", response_size);
        return FALSE;
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

    if (g_strcmp0(argv[0], "shuffle") == 0 && argc == 2) {
        bool enabled = false;
        if (parse_on_off(argv[1], &enabled)) {
            out_command->type = CONTROL_CMD_SHUFFLE;
            out_command->bool_value = enabled;
            return CONTROL_PARSE_OK;
        }
        return CONTROL_PARSE_INVALID;
    }

    if (g_strcmp0(argv[0], "overlay") == 0 && argc == 2) {
        bool enabled = false;
        if (parse_on_off(argv[1], &enabled)) {
            out_command->type = CONTROL_CMD_OVERLAY;
            out_command->bool_value = enabled;
            return CONTROL_PARSE_OK;
        }
        return CONTROL_PARSE_INVALID;
    }

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

    if (g_strcmp0(argv[0], "fps") == 0 && argc == 2) {
        long parsed = 0;
        if (!parse_long_range(argv[1], 10, 144, &parsed))
            return CONTROL_PARSE_INVALID;

        out_command->type = CONTROL_CMD_FPS;
        out_command->int_value = (int)parsed;
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "rotation-interval") == 0 && argc == 2) {
        long parsed = 0;
        if (!parse_long_range(argv[1], 5, 300, &parsed))
            return CONTROL_PARSE_INVALID;

        out_command->type = CONTROL_CMD_ROTATION_INTERVAL;
        out_command->int_value = (int)parsed;
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "save-state") == 0 && argc == 1) {
        out_command->type = CONTROL_CMD_SAVE_STATE;
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "restore-state") == 0 && argc >= 1) {
        out_command->type = CONTROL_CMD_RESTORE_STATE;
        if (argc >= 2)
            g_strlcpy(out_command->text_value, argv[1], sizeof(out_command->text_value));
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