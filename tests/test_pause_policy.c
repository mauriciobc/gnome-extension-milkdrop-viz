/**
 * test_pause_policy.c
 *
 * Tests the control-socket pause command path as used by the extension's
 * pause-policy logic (PausePolicy / _setPauseReason).
 *
 * The PausePolicy JS class sends `pause on` / `pause off` commands over the
 * control socket whenever visibility conditions change (fullscreen, maximized).
 * These tests verify the C binary handles those command sequences correctly.
 */

#include <glib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "app.h"
#include "control.h"

static void
pause_policy_setup_app(AppData *d)
{
    memset(d, 0, sizeof *d);
    g_mutex_init(&d->preset_dir_lock);
    g_mutex_init(&d->load_preset_lock);
}

static void
pause_policy_teardown_app(AppData *d)
{
    control_cleanup(d);
    g_free(d->socket_path);
    d->socket_path = NULL;
    g_mutex_clear(&d->preset_dir_lock);
    g_mutex_clear(&d->load_preset_lock);
}

static gchar *
send_cmd(const char *socket_path, const char *command)
{
    struct sockaddr_un addr = {0};
    char response[512] = {0};

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(fd, >=, 0);

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
    g_assert_cmpint(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    g_assert_cmpint(send(fd, command, strlen(command), 0), >, 0);

    ssize_t n = recv(fd, response, sizeof(response) - 1, 0);
    g_assert_cmpint(n, >, 0);
    response[n] = '\0';

    close(fd);
    return g_strdup(response);
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void
test_pause_on_sets_pause_audio(void)
{
    AppData d;
    pause_policy_setup_app(&d);
    atomic_store(&d.pause_audio, false);

    g_assert_true(control_init(&d));

    g_autofree gchar *resp = send_cmd(d.socket_path, "pause on\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_true(atomic_load(&d.pause_audio));

    pause_policy_teardown_app(&d);
}

static void
test_resume_clears_pause_audio(void)
{
    AppData d;
    pause_policy_setup_app(&d);
    atomic_store(&d.pause_audio, true);

    g_assert_true(control_init(&d));

    g_autofree gchar *resp = send_cmd(d.socket_path, "pause off\n");
    g_assert_true(g_str_has_prefix(resp, "ok"));
    g_assert_false(atomic_load(&d.pause_audio));

    pause_policy_teardown_app(&d);
}

static void
test_status_shows_paused_after_policy_pause(void)
{
    AppData d;
    pause_policy_setup_app(&d);
    atomic_store(&d.pause_audio, false);

    g_assert_true(control_init(&d));

    /* Policy sends pause on (e.g. fullscreen window appeared). */
    g_autofree gchar *r1 = send_cmd(d.socket_path, "pause on\n");
    g_assert_true(g_str_has_prefix(r1, "ok"));

    /* Status must reflect paused=1. */
    g_autofree gchar *status = send_cmd(d.socket_path, "status\n");
    g_assert_nonnull(strstr(status, "paused=1"));

    pause_policy_teardown_app(&d);
}

static void
test_status_shows_unpaused_after_resume(void)
{
    AppData d;
    pause_policy_setup_app(&d);
    atomic_store(&d.pause_audio, true);

    g_assert_true(control_init(&d));

    /* All pause reasons cleared — policy sends pause off. */
    g_autofree gchar *r1 = send_cmd(d.socket_path, "pause off\n");
    g_assert_true(g_str_has_prefix(r1, "ok"));

    g_autofree gchar *status = send_cmd(d.socket_path, "status\n");
    g_assert_nonnull(strstr(status, "paused=0"));

    pause_policy_teardown_app(&d);
}

static void
test_pause_toggle_sequence(void)
{
    /* Simulates the PausePolicy sending multiple pause on/off as
     * conditions change: fullscreen appears, then disappears. */
    AppData d;
    pause_policy_setup_app(&d);
    atomic_store(&d.pause_audio, false);

    g_assert_true(control_init(&d));

    /* fullscreen appeared → pause on */
    send_cmd(d.socket_path, "pause on\n");
    g_assert_true(atomic_load(&d.pause_audio));

    /* fullscreen left → pause off */
    g_autofree gchar *r = send_cmd(d.socket_path, "pause off\n");
    g_assert_true(g_str_has_prefix(r, "ok"));
    g_assert_false(atomic_load(&d.pause_audio));

    /* another fullscreen → pause on again */
    send_cmd(d.socket_path, "pause on\n");
    g_assert_true(atomic_load(&d.pause_audio));

    pause_policy_teardown_app(&d);
}

static void
test_last_pause_command_wins(void)
{
    /* The _setPauseReason pattern sends exactly one final command.
     * Verify the last command received is the one that counts. */
    AppData d;
    pause_policy_setup_app(&d);
    atomic_store(&d.pause_audio, false);

    g_assert_true(control_init(&d));

    send_cmd(d.socket_path, "pause on\n");
    send_cmd(d.socket_path, "pause on\n");   /* idempotent duplicate */
    send_cmd(d.socket_path, "pause off\n");  /* final state: unpaused */

    g_assert_false(atomic_load(&d.pause_audio));

    pause_policy_teardown_app(&d);
}

static void
test_status_includes_quarantine_field(void)
{
    /* Quarantine field (Phase 1) must still be present in Phase 2 status. */
    AppData d;
    pause_policy_setup_app(&d);

    g_assert_true(control_init(&d));

    g_autofree gchar *status = send_cmd(d.socket_path, "status\n");
    g_assert_nonnull(strstr(status, "quarantine=0"));

    pause_policy_teardown_app(&d);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/pause-policy/pause-on-sets-atomic",
                    test_pause_on_sets_pause_audio);
    g_test_add_func("/pause-policy/resume-clears-atomic",
                    test_resume_clears_pause_audio);
    g_test_add_func("/pause-policy/status-shows-paused",
                    test_status_shows_paused_after_policy_pause);
    g_test_add_func("/pause-policy/status-shows-unpaused",
                    test_status_shows_unpaused_after_resume);
    g_test_add_func("/pause-policy/toggle-sequence",
                    test_pause_toggle_sequence);
    g_test_add_func("/pause-policy/last-command-wins",
                    test_last_pause_command_wins);
    g_test_add_func("/pause-policy/status-includes-quarantine",
                    test_status_includes_quarantine_field);

    return g_test_run();
}
