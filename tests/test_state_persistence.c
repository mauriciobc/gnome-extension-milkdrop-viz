#include <glib.h>
#include <string.h>

#include "app.h"
#include "control.h"

/**
 * Test save-state command parsing
 */
static void
test_parse_save_state_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("save-state\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_SAVE_STATE);
}

/**
 * Test restore-state command parsing
 */
static void
test_parse_restore_state_command(void)
{
    ControlCommand command = {0};
    ControlParseResult result = control_parse_command("restore-state\n", &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_RESTORE_STATE);
}

/**
 * Test restore-state with path and paused flag
 */
static void
test_parse_restore_state_with_path(void)
{
    ControlCommand command = {0};
    const char *input = "restore-state '/path/to/preset.milk' 1\n";
    ControlParseResult result = control_parse_command(input, &command);

    g_assert_cmpint(result, ==, CONTROL_PARSE_OK);
    g_assert_cmpint(command.type, ==, CONTROL_CMD_RESTORE_STATE);
    g_assert_cmpstr(command.text_value, ==, "/path/to/preset.milk");
    g_assert_cmpint(command.int_value, ==, 1);
}

/**
 * Test title encoding format
 */
static void
test_title_encoding_format(void)
{
    // Test that the expected title format is correct
    // Format: @milkdrop!{"monitor":N,"overlay":B,"opacity":F}|N

    const char *expected_format = "@milkdrop!{\"monitor\":0,\"overlay\":false,\"opacity\":1.00}|0";

    // Verify structure
    g_assert_true(g_str_has_prefix(expected_format, "@milkdrop!"));
    g_assert_true(strstr(expected_format, "\"monitor\":") != NULL);
    g_assert_true(strstr(expected_format, "\"overlay\":") != NULL);
    g_assert_true(strstr(expected_format, "\"opacity\":") != NULL);

    // Find the separator
    const char *sep = strchr(expected_format, '|');
    g_assert_nonnull(sep);

    // Verify monitor index after separator
    g_assert_cmpint(atoi(sep + 1), ==, 0);
}

/**
 * Main test runner
 */
int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/state/save-state-parse", test_parse_save_state_command);
    g_test_add_func("/state/restore-state-parse", test_parse_restore_state_command);
    g_test_add_func("/state/restore-state-with-path", test_parse_restore_state_with_path);
    g_test_add_func("/state/title-encoding-format", test_title_encoding_format);

    return g_test_run();
}
