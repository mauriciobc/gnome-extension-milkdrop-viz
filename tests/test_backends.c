#include <glib.h>
#include <glib/gstdio.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "audio.h"
#include "control.h"
#include "renderer.h"

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
test_audio_init_initializes_ring(void)
{
    AppData app_data = {0};

    g_assert_true(audio_init(&app_data));
    g_assert_cmpuint(atomic_load(&app_data.ring.read_index), ==, 0u);
    g_assert_cmpuint(atomic_load(&app_data.ring.write_index), ==, 0u);

    audio_cleanup(&app_data);
}

static void
test_control_init_creates_socket_path(void)
{
    AppData app_data = {0};

    g_assert_true(control_init(&app_data));
    g_assert_nonnull(app_data.socket_path);
    /* Path now includes monitor index: milkdrop-N.sock */
    g_assert_nonnull(strstr(app_data.socket_path, "milkdrop-"));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_control_cleanup_unlinks_existing_socket_file(void)
{
    AppData app_data = {0};
    gchar* temp_file = g_strdup_printf("%s/milkdrop-test-%u.sock", g_get_tmp_dir(), g_random_int());

    g_assert_true(g_file_set_contents(temp_file, "x", 1, NULL));
    g_assert_true(g_file_test(temp_file, G_FILE_TEST_EXISTS));

    app_data.socket_path = temp_file;
    control_cleanup(&app_data);

    g_assert_false(g_file_test(temp_file, G_FILE_TEST_EXISTS));
    g_free(temp_file);
}

static void
test_control_init_listens_for_clients(void)
{
    AppData app_data = {0};
    struct sockaddr_un addr = {0};

    g_assert_true(control_init(&app_data));
    g_assert_nonnull(app_data.socket_path);
    g_assert_true(g_file_test(app_data.socket_path, G_FILE_TEST_EXISTS));

    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(client_fd, >=, 0);

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, app_data.socket_path, sizeof(addr.sun_path));
    g_assert_cmpint(connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)), ==, 0);

    close(client_fd);
    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_control_commands_update_runtime_state(void)
{
    AppData app_data = {0};

    g_assert_true(control_init(&app_data));

    g_autofree gchar* op_resp = send_control_command(app_data.socket_path, "opacity 0.42\n");
    g_assert_true(g_str_has_prefix(op_resp, "ok"));
    g_assert_cmpfloat(atomic_load(&app_data.opacity), ==, 0.42f);

    g_autofree gchar* pause_resp = send_control_command(app_data.socket_path, "pause on\n");
    g_assert_true(g_str_has_prefix(pause_resp, "ok"));
    g_assert_true(atomic_load(&app_data.pause_audio));

    g_autofree gchar* status_resp = send_control_command(app_data.socket_path, "status\n");
    g_assert_nonnull(strstr(status_resp, "opacity=0.420"));
    g_assert_nonnull(strstr(status_resp, "paused=1"));

    control_cleanup(&app_data);
    g_free(app_data.socket_path);
}

static void
test_socket_path_per_monitor(void)
{
    g_autofree gchar* path0 = control_socket_path_for_monitor(0);
    g_autofree gchar* path1 = control_socket_path_for_monitor(1);
    g_autofree gchar* path2 = control_socket_path_for_monitor(2);

    g_assert_nonnull(path0);
    g_assert_nonnull(path1);
    g_assert_nonnull(path2);

    g_assert_cmpstr(path0, !=, path1);
    g_assert_cmpstr(path1, !=, path2);
    g_assert_cmpstr(path0, !=, path2);
}

static void
test_socket_paths_include_monitor_index(void)
{
    g_autofree gchar* path3 = control_socket_path_for_monitor(3);

    g_assert_nonnull(path3);
    g_assert_nonnull(strstr(path3, "3"));
}

static void
test_on_resize_sets_render_dimensions(void)
{
    AppData app_data = {0};
    /* Call renderer_apply_resize simulating a 1920x1080 widget with scale factor 2 */
    renderer_apply_resize(&app_data, 1920, 1080, 2);

    g_assert_cmpint(app_data.render_width, ==, 3840);
    g_assert_cmpint(app_data.render_height, ==, 2160);
}

static void
test_render_skipped_when_size_is_zero(void)
{
    AppData app_data = {0};
    /* Simulate a 0x0 widget size initially */
    renderer_apply_resize(&app_data, 0, 0, 1);

    g_assert_cmpint(app_data.render_width, ==, 0);
    g_assert_cmpint(app_data.render_height, ==, 0);

    /* Resize to valid dimensions */
    renderer_apply_resize(&app_data, 1280, 720, 1);
    g_assert_cmpint(app_data.render_width, ==, 1280);
    g_assert_cmpint(app_data.render_height, ==, 720);
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/backends/audio-init", test_audio_init_initializes_ring);
    g_test_add_func("/backends/control-init", test_control_init_creates_socket_path);
    g_test_add_func("/backends/control-cleanup-unlinks", test_control_cleanup_unlinks_existing_socket_file);
    g_test_add_func("/backends/control-listens", test_control_init_listens_for_clients);
    g_test_add_func("/backends/control-commands", test_control_commands_update_runtime_state);
    g_test_add_func("/renderer/resize-sets-dimensions", test_on_resize_sets_render_dimensions);
    g_test_add_func("/renderer/resize-hidpi-scale", test_render_skipped_when_size_is_zero);
    g_test_add_func("/backends/socket-path-per-monitor", test_socket_path_per_monitor);
    g_test_add_func("/backends/socket-path-includes-index", test_socket_paths_include_monitor_index);

    return g_test_run();
}