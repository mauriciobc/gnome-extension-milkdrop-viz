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
    g_test_add_func("/control/parse-next", test_parse_next_command);
    g_test_add_func("/control/parse-previous", test_parse_previous_command);
    g_test_add_func("/control/parse-next-extra-args", test_parse_next_rejects_extra_args);
    g_test_add_func("/control/parse-previous-extra-args", test_parse_previous_rejects_extra_args);

    return g_test_run();
}
