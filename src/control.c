#include "control.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static gboolean
control_apply_command(AppData* app_data, const ControlCommand* command, gchar* response, gsize response_size)
{
    switch (command->type) {
    case CONTROL_CMD_STATUS: {
        int audio_fails = atomic_load(&app_data->audio_fail_count);
        const char *audio_status = (audio_fails == 0)                   ? "ok"
                                   : (audio_fails < AUDIO_MAX_RESTARTS) ? "recovering"
                                                                         : "failed";
        g_snprintf(response,
                   response_size,
                   "paused=%d\nopacity=%.3f\nshuffle=%d\noverlay=%d\nquarantine=%d\naudio=%s\nfps=%.1f\npreset=%s\n\n",
                   atomic_load(&app_data->pause_audio) ? 1 : 0,
                   atomic_load(&app_data->opacity),
                   atomic_load(&app_data->shuffle_runtime) ? 1 : 0,
                   atomic_load(&app_data->overlay_enabled) ? 1 : 0,
                   app_data->quarantine_count,
                   audio_status,
                   atomic_load(&app_data->fps_last),
                   app_data->last_good_preset);
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
    case CONTROL_CMD_NONE:
    default:
        g_strlcpy(response, "err invalid\n", response_size);
        return FALSE;
    }
}

static void
control_handle_client(AppData* app_data, int client_fd)
{
    char buffer[1024] = {0};
    size_t used = 0;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (used < sizeof(buffer) - 1) {
        ssize_t n = recv(client_fd, buffer + used, sizeof(buffer) - 1 - used, 0);
        if (n <= 0)
            break;
        used += (size_t)n;

        if (memchr(buffer, '\n', used))
            break;
    }

    if (used == 0)
        return;

    buffer[used] = '\0';

    ControlCommand command = {0};
    ControlParseResult parse_result = control_parse_command(buffer, &command);
    gchar response[MILKDROP_PATH_MAX + 256] = {0};

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
        char* endptr = NULL;
        errno = 0;
        double parsed = g_ascii_strtod(argv[1], &endptr);
        if (errno != 0 || !endptr || *endptr != '\0' || parsed < 0.0 || parsed > 1.0)
            return CONTROL_PARSE_INVALID;

        out_command->type = CONTROL_CMD_OPACITY;
        out_command->opacity = (float)parsed;
        return CONTROL_PARSE_OK;
    }

    if (g_strcmp0(argv[0], "pause") == 0 && argc == 2) {
        if (g_strcmp0(argv[1], "on") == 0) {
            out_command->type = CONTROL_CMD_PAUSE;
            out_command->pause_enabled = true;
            return CONTROL_PARSE_OK;
        }

        if (g_strcmp0(argv[1], "off") == 0) {
            out_command->type = CONTROL_CMD_PAUSE;
            out_command->pause_enabled = false;
            return CONTROL_PARSE_OK;
        }
    }

    if (g_strcmp0(argv[0], "shuffle") == 0 && argc == 2) {
        if (g_strcmp0(argv[1], "on") == 0 || g_strcmp0(argv[1], "off") == 0) {
            out_command->type = CONTROL_CMD_SHUFFLE;
            out_command->bool_value = g_strcmp0(argv[1], "on") == 0;
            return CONTROL_PARSE_OK;
        }
        return CONTROL_PARSE_INVALID;
    }

    if (g_strcmp0(argv[0], "overlay") == 0 && argc == 2) {
        if (g_strcmp0(argv[1], "on") == 0 || g_strcmp0(argv[1], "off") == 0) {
            out_command->type = CONTROL_CMD_OVERLAY;
            out_command->bool_value = g_strcmp0(argv[1], "on") == 0;
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

    app_data->control_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (app_data->control_fd < 0) {
        g_warning("Failed to create control socket: %s", g_strerror(errno));
        return false;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
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