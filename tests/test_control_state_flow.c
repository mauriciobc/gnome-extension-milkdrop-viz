#include <glib.h>
#include <glib/gstdio.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>

#include "app.h"
#include "control.h"

static gchar*
send_control_command(const char* socket_path, const char* command)
{
    struct sockaddr_un addr = {0};
    char response[256] = {0};

    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(client_fd, >=, 0);

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
    g_assert_cmpint(connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)), ==, 0);

    g_assert_cmpint(send(client_fd, command, strlen(command), 0), >, 0);

    ssize_t n = recv(client_fd, response, sizeof(response) - 1, 0);
    g_assert_cmpint(n, >, 0);
    response[n] = '\0';

    close(client_fd);
    return g_strdup(response);
}

static void
test_opacity_command_updates_atomic(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.opacity, 1.0f);

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "opacity 0.42\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_cmpfloat(atomic_load(&app_data.opacity), ==, 0.42f);

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_pause_on_command_updates_atomic(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.pause_audio, false);

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "pause on\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.pause_audio));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_pause_off_command_updates_atomic(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.pause_audio, true);

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "pause off\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_false(atomic_load(&app_data.pause_audio));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_shuffle_command_updates_atomic(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.shuffle_runtime, false);

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "shuffle on\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.shuffle_runtime));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_overlay_command_updates_atomic(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.overlay_enabled, false);

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "overlay on\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.overlay_enabled));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_preset_dir_command_sets_pending(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    atomic_store(&app_data.preset_dir_pending, false);

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "preset-dir /tmp/new-presets\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.preset_dir_pending));
    g_assert_cmpstr(app_data.pending_preset_dir, ==, "/tmp/new-presets");

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
    g_mutex_clear(&app_data.preset_dir_lock);
}

static void
test_next_command_sets_pending(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.next_preset_pending, false);
    atomic_store(&app_data.prev_preset_pending, false);

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "next\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.next_preset_pending));
    g_assert_false(atomic_load(&app_data.prev_preset_pending));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_previous_command_sets_pending(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.next_preset_pending, false);
    atomic_store(&app_data.prev_preset_pending, false);

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "previous\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&app_data.prev_preset_pending));
    g_assert_false(atomic_load(&app_data.next_preset_pending));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_status_command_reflects_current_state(void)
{
    AppData app_data = {0};
    atomic_store(&app_data.opacity, 0.75f);
    atomic_store(&app_data.pause_audio, true);
    atomic_store(&app_data.shuffle_runtime, false);
    atomic_store(&app_data.overlay_enabled, true);

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "status\n");
    g_assert_nonnull(strstr(resp, "opacity=0.750"));
    g_assert_nonnull(strstr(resp, "paused=1"));
    g_assert_nonnull(strstr(resp, "shuffle=0"));
    g_assert_nonnull(strstr(resp, "overlay=1"));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_invalid_command_returns_error(void)
{
    AppData app_data = {0};

    g_assert_true(control_init(&app_data));

    g_autofree gchar* resp = send_control_command(app_data.socket_path, "bogus\n");
    g_assert_true(g_str_has_prefix(resp, "err"));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_full_parse_and_apply_cycle(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    atomic_store(&app_data.opacity, 1.0f);
    atomic_store(&app_data.pause_audio, false);

    g_assert_true(control_init(&app_data));

    /* Send opacity command. */
    g_autofree gchar* op_resp = send_control_command(app_data.socket_path, "opacity 0.33\n");
    g_assert_true(g_str_has_prefix(op_resp, "ok"));
    g_assert_cmpfloat(atomic_load(&app_data.opacity), ==, 0.33f);

    /* Verify via status. */
    g_autofree gchar* status_resp = send_control_command(app_data.socket_path, "status\n");
    g_assert_nonnull(strstr(status_resp, "opacity=0.330"));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
    g_mutex_clear(&app_data.preset_dir_lock);
}

static void
test_multiple_commands_sequence(void)
{
    AppData app_data = {0};
    g_mutex_init(&app_data.preset_dir_lock);
    atomic_store(&app_data.opacity, 1.0f);
    atomic_store(&app_data.pause_audio, false);
    atomic_store(&app_data.shuffle_runtime, false);
    atomic_store(&app_data.overlay_enabled, false);

    g_assert_true(control_init(&app_data));

    /* Send a sequence of commands. */
    g_autofree gchar* r1 = send_control_command(app_data.socket_path, "opacity 0.5\n");
    g_assert_true(g_str_has_prefix(r1, "ok"));

    g_autofree gchar* r2 = send_control_command(app_data.socket_path, "pause on\n");
    g_assert_true(g_str_has_prefix(r2, "ok"));

    g_autofree gchar* r3 = send_control_command(app_data.socket_path, "shuffle on\n");
    g_assert_true(g_str_has_prefix(r3, "ok"));

    g_autofree gchar* r4 = send_control_command(app_data.socket_path, "overlay on\n");
    g_assert_true(g_str_has_prefix(r4, "ok"));

    /* Verify all states via status. */
    g_autofree gchar* status = send_control_command(app_data.socket_path, "status\n");
    g_assert_nonnull(strstr(status, "opacity=0.500"));
    g_assert_nonnull(strstr(status, "paused=1"));
    g_assert_nonnull(strstr(status, "shuffle=1"));
    g_assert_nonnull(strstr(status, "overlay=1"));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
    g_mutex_clear(&app_data.preset_dir_lock);
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
    g_test_add_func("/control-state/previous-preset-pending", test_previous_command_sets_pending);
    g_test_add_func("/control-state/status-reflects-state", test_status_command_reflects_current_state);
    g_test_add_func("/control-state/invalid-command", test_invalid_command_returns_error);
    g_test_add_func("/control-state/parse-and-apply-cycle", test_full_parse_and_apply_cycle);
    g_test_add_func("/control-state/multiple-commands-sequence", test_multiple_commands_sequence);

    return g_test_run();
}
