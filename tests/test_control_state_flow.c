#include <glib.h>
#include <glib/gstdio.h>

#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "app.h"
#include "control.h"

#define TEST_CONTROL_MESSAGE_MAX (MILKDROP_PATH_MAX * 5)

static gchar*
send_control_command(const char* socket_path, const char* command)
{
    struct sockaddr_un addr = {0};
    char response[TEST_CONTROL_MESSAGE_MAX] = {0};
    size_t used = 0;

    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(client_fd, >=, 0);

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
    g_assert_cmpint(connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)), ==, 0);

    g_assert_cmpint(send(client_fd, command, strlen(command), 0), >, 0);
    shutdown(client_fd, SHUT_WR);

    while (used < sizeof(response) - 1) {
        ssize_t n = recv(client_fd, response + used, sizeof(response) - 1 - used, 0);
        g_assert_cmpint(n, >=, 0);
        if (n == 0)
            break;

        used += (size_t)n;
    }

    g_assert_cmpint(used, >, 0);
    response[used] = '\0';

    close(client_fd);
    return g_strdup(response);
}

static void
init_test_app_data(AppData* app_data)
{
    *app_data = (AppData){0};
    g_mutex_init(&app_data->preset_dir_lock);
    g_mutex_init(&app_data->load_preset_lock);
    app_data->pending_preset_dir[0] = '\0';
    app_data->pending_load_preset[0] = '\0';
    atomic_store(&app_data->opacity, 1.0f);
    atomic_store(&app_data->pause_audio, false);
    atomic_store(&app_data->shuffle_runtime, false);
    atomic_store(&app_data->overlay_enabled, false);
    atomic_store(&app_data->preset_dir_pending, false);
    atomic_store(&app_data->next_preset_pending, 0u);
    atomic_store(&app_data->prev_preset_pending, 0u);
    g_assert_true(control_init(app_data));
}

static void
free_test_app_data(AppData* app_data)
{
    if (!app_data)
        return;

    control_cleanup(app_data);
    g_clear_pointer(&app_data->preset_dir, g_free);
    g_clear_pointer(&app_data->socket_path, g_free);
    g_mutex_clear(&app_data->preset_dir_lock);
    g_mutex_clear(&app_data->load_preset_lock);
}

static void
test_opacity_command_updates_atomic(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "opacity 0.42\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_cmpfloat(atomic_load(&app_data.opacity), ==, 0.42f);

    free_test_app_data(&app_data);
}

static void
test_pause_on_command_updates_atomic(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "pause on\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.pause_audio));

    free_test_app_data(&app_data);
}

static void
test_pause_off_command_updates_atomic(void)
{
    AppData app_data;
    init_test_app_data(&app_data);
    atomic_store(&app_data.pause_audio, true);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "pause off\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_false(atomic_load(&app_data.pause_audio));

    free_test_app_data(&app_data);
}

static void
test_shuffle_command_updates_atomic(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "shuffle on\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.shuffle_runtime));

    free_test_app_data(&app_data);
}

static void
test_overlay_command_updates_atomic(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "overlay on\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.overlay_enabled));

    free_test_app_data(&app_data);
}

static void
test_preset_dir_command_sets_pending(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "preset-dir /tmp/new-presets\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.preset_dir_pending));
    g_assert_cmpstr(app_data.pending_preset_dir, ==, "/tmp/new-presets");

    free_test_app_data(&app_data);
}

static void
test_next_command_sets_pending(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "next\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_cmpuint(atomic_load(&app_data.next_preset_pending), ==, 1u);
    g_assert_cmpuint(atomic_load(&app_data.prev_preset_pending), ==, 0u);

    free_test_app_data(&app_data);
}

static void
test_previous_command_sets_pending(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "previous\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_cmpuint(atomic_load(&app_data.prev_preset_pending), ==, 1u);
    g_assert_cmpuint(atomic_load(&app_data.next_preset_pending), ==, 0u);

    free_test_app_data(&app_data);
}

static void
test_next_command_accumulates(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    for (int i = 0; i < 3; i++) {
        g_autofree gchar* resp = send_control_command(app_data.socket_path, "next\n");
        g_assert_true(g_str_has_prefix(resp, "ok"));
    }
    g_assert_cmpuint(atomic_load(&app_data.next_preset_pending), ==, 3u);

    free_test_app_data(&app_data);
}

static void
test_status_command_reflects_current_state(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    atomic_store(&app_data.opacity, 0.75f);
    atomic_store(&app_data.pause_audio, true);
    atomic_store(&app_data.overlay_enabled, true);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "status\n");
    g_assert_nonnull(strstr(resp, "opacity=0.750"));
    g_assert_nonnull(strstr(resp, "paused=1"));
    g_assert_nonnull(strstr(resp, "shuffle=0"));
    g_assert_nonnull(strstr(resp, "overlay=1"));

    free_test_app_data(&app_data);
}

static void
test_invalid_command_returns_error(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "bogus\n");
    g_assert_true(g_str_has_prefix(resp, "err"));

    free_test_app_data(&app_data);
}

static void
test_full_parse_and_apply_cycle(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* op_resp = send_control_command(app_data.socket_path, "opacity 0.33\n");
    g_assert_true(g_str_has_prefix(op_resp, "ok"));
    g_assert_cmpfloat(atomic_load(&app_data.opacity), ==, 0.33f);

    g_autofree gchar* status_resp = send_control_command(app_data.socket_path, "status\n");
    g_assert_nonnull(strstr(status_resp, "opacity=0.330"));

    free_test_app_data(&app_data);
}

static void
test_multiple_commands_sequence(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* r1 = send_control_command(app_data.socket_path, "opacity 0.5\n");
    g_assert_true(g_str_has_prefix(r1, "ok"));

    g_autofree gchar* r2 = send_control_command(app_data.socket_path, "pause on\n");
    g_assert_true(g_str_has_prefix(r2, "ok"));

    g_autofree gchar* r3 = send_control_command(app_data.socket_path, "shuffle on\n");
    g_assert_true(g_str_has_prefix(r3, "ok"));

    g_autofree gchar* r4 = send_control_command(app_data.socket_path, "overlay on\n");
    g_assert_true(g_str_has_prefix(r4, "ok"));

    g_autofree gchar* status = send_control_command(app_data.socket_path, "status\n");
    g_assert_nonnull(strstr(status, "opacity=0.500"));
    g_assert_nonnull(strstr(status, "paused=1"));
    g_assert_nonnull(strstr(status, "shuffle=1"));
    g_assert_nonnull(strstr(status, "overlay=1"));

    free_test_app_data(&app_data);
}

static void
test_restore_state_updates_runtime_state(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* resp = send_control_command(
        app_data.socket_path,
        "restore-state paused=on opacity=0.250 shuffle=on preset-dir='/tmp/restore dir,comma'\n");

    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.pause_audio));
    g_assert_cmpfloat(atomic_load(&app_data.opacity), ==, 0.25f);
    g_assert_true(atomic_load(&app_data.shuffle_runtime));
    g_assert_true(atomic_load(&app_data.preset_dir_pending));
    g_assert_cmpstr(app_data.pending_preset_dir, ==, "/tmp/restore dir,comma");

    free_test_app_data(&app_data);
}

static void
test_save_state_omits_empty_preset_dir(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    atomic_store(&app_data.pause_audio, true);
    atomic_store(&app_data.opacity, 0.625f);
    atomic_store(&app_data.shuffle_runtime, true);

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "save-state\n");
    g_assert_nonnull(strstr(resp, "paused=on"));
    g_assert_nonnull(strstr(resp, "opacity=0.625"));
    g_assert_nonnull(strstr(resp, "shuffle=on"));
    g_assert_null(strstr(resp, "preset-dir="));

    free_test_app_data(&app_data);
}

static void
test_save_state_restore_state_round_trip(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    app_data.preset_dir = g_strdup("/tmp/state dir,comma:colon");
    atomic_store(&app_data.pause_audio, true);
    atomic_store(&app_data.opacity, 0.875f);
    atomic_store(&app_data.shuffle_runtime, true);

    g_autofree gchar* saved_state = send_control_command(app_data.socket_path, "save-state\n");
    g_assert_nonnull(strstr(saved_state, "preset-dir='"));

    atomic_store(&app_data.pause_audio, false);
    atomic_store(&app_data.opacity, 0.100f);
    atomic_store(&app_data.shuffle_runtime, false);
    atomic_store(&app_data.preset_dir_pending, false);
    g_strlcpy(app_data.pending_preset_dir, "/tmp/old", sizeof(app_data.pending_preset_dir));

    g_autofree gchar* restore_command = g_strdup_printf("restore-state %s", saved_state);
    g_autofree gchar* restore_resp = send_control_command(app_data.socket_path, restore_command);

    g_assert_true(g_str_has_prefix(restore_resp, "ok"));
    g_assert_true(atomic_load(&app_data.pause_audio));
    g_assert_cmpfloat(atomic_load(&app_data.opacity), ==, 0.875f);
    g_assert_true(atomic_load(&app_data.shuffle_runtime));
    g_assert_true(atomic_load(&app_data.preset_dir_pending));
    g_assert_cmpstr(app_data.pending_preset_dir, ==, "/tmp/state dir,comma:colon");

    free_test_app_data(&app_data);
}

static void
test_save_state_handles_long_quoted_preset_dir(void)
{
    AppData app_data;
    init_test_app_data(&app_data);

    g_autofree gchar* prefix = g_strdup("/tmp/");
    g_autofree gchar* repeated = g_strnfill(1500, 'q');
    g_clear_pointer(&app_data.preset_dir, g_free);
    app_data.preset_dir = g_strconcat(prefix, "long dir,", repeated, NULL);

    g_autofree gchar* saved_state = send_control_command(app_data.socket_path, "save-state\n");
    g_assert_nonnull(strstr(saved_state, "preset-dir='"));

    g_autofree gchar* restore_command = g_strdup_printf("restore-state %s", saved_state);
    g_autofree gchar* restore_resp = send_control_command(app_data.socket_path, restore_command);

    g_assert_true(g_str_has_prefix(restore_resp, "ok"));
    g_assert_true(atomic_load(&app_data.preset_dir_pending));
    g_assert_cmpstr(app_data.pending_preset_dir, ==, app_data.preset_dir);

    free_test_app_data(&app_data);
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/control-state/opacity", test_opacity_command_updates_atomic);
    g_test_add_func("/control-state/pause-on", test_pause_on_command_updates_atomic);
    g_test_add_func("/control-state/pause-off", test_pause_off_command_updates_atomic);
    g_test_add_func("/control-state/shuffle", test_shuffle_command_updates_atomic);
    g_test_add_func("/control-state/overlay", test_overlay_command_updates_atomic);
    g_test_add_func("/control-state/preset-dir-pending", test_preset_dir_command_sets_pending);
    g_test_add_func("/control-state/next-preset-pending", test_next_command_sets_pending);
    g_test_add_func("/control-state/next-preset-accumulates", test_next_command_accumulates);
    g_test_add_func("/control-state/previous-preset-pending", test_previous_command_sets_pending);
    g_test_add_func("/control-state/status-reflects-state", test_status_command_reflects_current_state);
    g_test_add_func("/control-state/invalid-command", test_invalid_command_returns_error);
    g_test_add_func("/control-state/parse-and-apply-cycle", test_full_parse_and_apply_cycle);
    g_test_add_func("/control-state/multiple-commands-sequence", test_multiple_commands_sequence);
    g_test_add_func("/control-state/restore-state-runtime", test_restore_state_updates_runtime_state);
    g_test_add_func("/control-state/save-state-omit-empty-preset-dir", test_save_state_omits_empty_preset_dir);
    g_test_add_func("/control-state/save-restore-round-trip", test_save_state_restore_state_round_trip);
    g_test_add_func("/control-state/save-state-long-preset-dir", test_save_state_handles_long_quoted_preset_dir);

    return g_test_run();
}
