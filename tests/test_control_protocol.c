#include <glib.h>
#include <string.h>

#include "app.h"
#include "control.h"

static void
test_parse_status_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("status\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_STATUS);
}

static void
test_parse_opacity_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("opacity 0.5\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_OPACITY);
    g_assert_cmpfloat(command.opacity, ==, 0.5f);
}

static void
test_parse_pause_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("pause on\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_PAUSE);
    g_assert_true(command.pause_enabled);
}

static void
test_parse_invalid_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("invalid\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_malformed_arguments(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("opacity nope\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_partial_line(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("status", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INCOMPLETE);
}

static void
test_parse_shuffle_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("shuffle off\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_SHUFFLE);
    g_assert_false(command.bool_value);
}

static void
test_parse_overlay_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("overlay on\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_OVERLAY);
    g_assert_true(command.bool_value);
}

static void
test_parse_preset_dir_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("preset-dir /tmp/presets\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_PRESET_DIR);
    g_assert_cmpstr(command.text_value, ==, "/tmp/presets");
}

static void
test_parse_restore_state_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("restore-state\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_RESTORE_STATE);
    g_assert_false(command.restore_has_preset_dir);
    g_assert_false(command.restore_has_pause);
    g_assert_false(command.restore_has_opacity);
    g_assert_false(command.restore_has_shuffle);
}

static void
test_parse_restore_state_with_values(void)
{
    ControlCommand command = {0};
    const char* input =
        "restore-state preset-dir='/tmp/space dir,comma:colon' paused=on opacity=0.375 shuffle=off\n";
    ControlParseResult result = control_parse_command(input, &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_RESTORE_STATE);
    g_assert_true(command.restore_has_preset_dir);
    g_assert_cmpstr(command.restore_preset_dir, ==, "/tmp/space dir,comma:colon");
    g_assert_true(command.restore_has_pause);
    g_assert_true(command.restore_pause_enabled);
    g_assert_true(command.restore_has_opacity);
    g_assert_cmpfloat(command.restore_opacity, ==, 0.375f);
    g_assert_true(command.restore_has_shuffle);
    g_assert_false(command.restore_shuffle_enabled);
}

static void
test_parse_restore_state_rejects_unknown_key(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("restore-state bogus=1\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_restore_state_rejects_bad_boolean(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("restore-state paused=yes\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_restore_state_rejects_bad_opacity(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("restore-state opacity=1.5\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_restore_state_rejects_empty_value(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("restore-state paused=\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_restore_state_rejects_too_long_preset_dir(void)
{
    g_autofree char* path = g_malloc((gsize)MILKDROP_PATH_MAX + 8);
    memset(path, 'p', (size_t)MILKDROP_PATH_MAX);
    path[MILKDROP_PATH_MAX] = '\0';

    g_autofree char* quoted = g_shell_quote(path);
    g_autofree char* line = g_strdup_printf("restore-state preset-dir=%s\n", quoted);

    ControlCommand command = {0};
    ControlParseResult result = control_parse_command(line, &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_preset_dir_max_length(void)
{
    g_autofree char* path = g_malloc(MILKDROP_PATH_MAX);
    memset(path, 'z', (size_t)MILKDROP_PATH_MAX - 1);
    path[MILKDROP_PATH_MAX - 1] = '\0';

    g_autofree char* line = g_strdup_printf("preset-dir %s\n", path);
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command(line, &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_PRESET_DIR);
    g_assert_cmpstr(command.text_value, ==, path);
}

static void
test_parse_preset_dir_path_too_long(void)
{
    g_autofree char* path = g_malloc((gsize)MILKDROP_PATH_MAX + 8);
    memset(path, 'y', (size_t)MILKDROP_PATH_MAX);
    path[MILKDROP_PATH_MAX] = '\0';

    g_autofree char* line = g_strdup_printf("preset-dir %s\n", path);
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command(line, &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_PATH_TOO_LONG);
}

static void
test_parse_next_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("next\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_NEXT_PRESET);
}

static void
test_parse_previous_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("previous\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_PREV_PRESET);
}

static void
test_parse_next_rejects_extra_args(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("next now\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_previous_rejects_extra_args(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("previous now\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_fps_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("fps 30\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_FPS);
    g_assert_cmpint(command.int_value, ==, 30);
}

static void
test_parse_fps_out_of_range_low(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("fps 0\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_fps_out_of_range_high(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("fps 999\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_INVALID);
}

static void
test_parse_status_response_fps(void)
{
    const char* response =
        "fps=58.3\npaused=0\npreset=\naudio=ok\nquarantine=0\n\n";
    StatusResponse sr = {0};

    g_assert_true(status_response_parse(response, &sr));
    g_assert_cmpfloat_with_epsilon(sr.fps, 58.3f, 0.01f);
}

static void
test_parse_status_response_paused(void)
{
    const char* response =
        "fps=0.0\npaused=1\npreset=\naudio=ok\nquarantine=0\n\n";
    StatusResponse sr = {0};

    g_assert_true(status_response_parse(response, &sr));
    g_assert_true(sr.paused);
}

static void
test_parse_status_response_preset(void)
{
    const char* response =
        "fps=0.0\npaused=0\npreset=foo.milk\naudio=ok\nquarantine=0\n\n";
    StatusResponse sr = {0};

    g_assert_true(status_response_parse(response, &sr));
    g_assert_cmpstr(sr.preset, ==, "foo.milk");
}

static void
test_parse_status_response_audio(void)
{
    const char* response =
        "fps=0.0\npaused=0\npreset=\naudio=recovering\nquarantine=0\n\n";
    StatusResponse sr = {0};

    g_assert_true(status_response_parse(response, &sr));
    g_assert_cmpint(sr.audio, ==, CONTROL_AUDIO_RECOVERING);
}

static void
test_parse_status_response_quarantine(void)
{
    const char* response =
        "fps=0.0\npaused=0\npreset=\naudio=ok\nquarantine=2\n\n";
    StatusResponse sr = {0};

    g_assert_true(status_response_parse(response, &sr));
    g_assert_cmpint(sr.quarantine_count, ==, 2);
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/control/parse-status", test_parse_status_command);
    g_test_add_func("/control/parse-opacity", test_parse_opacity_command);
    g_test_add_func("/control/parse-pause", test_parse_pause_command);
    g_test_add_func("/control/parse-invalid", test_parse_invalid_command);
    g_test_add_func("/control/parse-malformed", test_parse_malformed_arguments);
    g_test_add_func("/control/parse-partial", test_parse_partial_line);
    g_test_add_func("/control/parse-shuffle", test_parse_shuffle_command);
    g_test_add_func("/control/parse-overlay", test_parse_overlay_command);
    g_test_add_func("/control/parse-preset-dir", test_parse_preset_dir_command);
    g_test_add_func("/control/parse-preset-dir-max-len", test_parse_preset_dir_max_length);
    g_test_add_func("/control/parse-preset-dir-too-long", test_parse_preset_dir_path_too_long);
    g_test_add_func("/control/parse-restore-state", test_parse_restore_state_command);
    g_test_add_func("/control/parse-restore-state-values", test_parse_restore_state_with_values);
    g_test_add_func("/control/parse-restore-state-unknown", test_parse_restore_state_rejects_unknown_key);
    g_test_add_func("/control/parse-restore-state-bad-boolean", test_parse_restore_state_rejects_bad_boolean);
    g_test_add_func("/control/parse-restore-state-bad-opacity", test_parse_restore_state_rejects_bad_opacity);
    g_test_add_func("/control/parse-restore-state-empty-value", test_parse_restore_state_rejects_empty_value);
    g_test_add_func("/control/parse-restore-state-too-long-preset-dir", test_parse_restore_state_rejects_too_long_preset_dir);
    g_test_add_func("/control/parse-next", test_parse_next_command);
    g_test_add_func("/control/parse-previous", test_parse_previous_command);
    g_test_add_func("/control/parse-next-extra-args", test_parse_next_rejects_extra_args);
    g_test_add_func("/control/parse-previous-extra-args", test_parse_previous_rejects_extra_args);
    g_test_add_func("/control/status-response-fps", test_parse_status_response_fps);
    g_test_add_func("/control/status-response-paused", test_parse_status_response_paused);
    g_test_add_func("/control/status-response-preset", test_parse_status_response_preset);
    g_test_add_func("/control/status-response-audio", test_parse_status_response_audio);
    g_test_add_func("/control/status-response-quarantine", test_parse_status_response_quarantine);
    g_test_add_func("/control/parse-fps", test_parse_fps_command);
    g_test_add_func("/control/parse-fps-low", test_parse_fps_out_of_range_low);
    g_test_add_func("/control/parse-fps-high", test_parse_fps_out_of_range_high);

    return g_test_run();
}
