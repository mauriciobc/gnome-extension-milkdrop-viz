#include <glib.h>
#include <string.h>

#include "app.h"
#include "control.h"

static void
test_parse_save_state_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("save-state\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_SAVE_STATE);
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
test_parse_restore_state_round_trip_payload(void)
{
    ControlCommand command = {0};
    const char* input =
        "restore-state preset-dir='/tmp/preset dir,comma:colon' paused=off opacity=0.500 shuffle=on\n";
    ControlParseResult result = control_parse_command(input, &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_RESTORE_STATE);
    g_assert_true(command.restore_has_preset_dir);
    g_assert_cmpstr(command.restore_preset_dir, ==, "/tmp/preset dir,comma:colon");
    g_assert_true(command.restore_has_pause);
    g_assert_false(command.restore_pause_enabled);
    g_assert_true(command.restore_has_opacity);
    g_assert_cmpfloat(command.restore_opacity, ==, 0.5f);
    g_assert_true(command.restore_has_shuffle);
    g_assert_true(command.restore_shuffle_enabled);
}

static void
test_parse_restore_state_without_preset_dir(void)
{
    ControlCommand command = {0};
    const char* input = "restore-state paused=on opacity=0.125 shuffle=off\n";
    ControlParseResult result = control_parse_command(input, &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_RESTORE_STATE);
    g_assert_false(command.restore_has_preset_dir);
    g_assert_true(command.restore_has_pause);
    g_assert_true(command.restore_pause_enabled);
    g_assert_true(command.restore_has_opacity);
    g_assert_cmpfloat(command.restore_opacity, ==, 0.125f);
    g_assert_true(command.restore_has_shuffle);
    g_assert_false(command.restore_shuffle_enabled);
}

static void
test_title_encoding_format(void)
{
    const char* expected_format = "@milkdrop!{\"monitor\":0,\"overlay\":false,\"opacity\":1.00}|0";

    g_assert_true(g_str_has_prefix(expected_format, "@milkdrop!"));
    g_assert_true(strstr(expected_format, "\"monitor\":") != NULL);
    g_assert_true(strstr(expected_format, "\"overlay\":") != NULL);
    g_assert_true(strstr(expected_format, "\"opacity\":") != NULL);

    const char* sep = strchr(expected_format, '|');
    g_assert_nonnull(sep);
    g_assert_cmpint(atoi(sep + 1), ==, 0);
}

int
main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/state/save-state-parse", test_parse_save_state_command);
    g_test_add_func("/state/restore-state-parse", test_parse_restore_state_command);
    g_test_add_func("/state/restore-state-round-trip-payload", test_parse_restore_state_round_trip_payload);
    g_test_add_func("/state/restore-state-without-preset-dir", test_parse_restore_state_without_preset_dir);
    g_test_add_func("/state/title-encoding-format", test_title_encoding_format);

    return g_test_run();
}
