#include <glib.h>

#include "app.h"
#include "startup_gate.h"

static void
test_request_keeps_final_content_inactive_until_confirmed(void)
{
    AppData app_data = {0};

    app_data.startup_deferred_preset_activation = true;
    app_data.startup_final_content_active = false;

    startup_gate_request_preset_activation(&app_data);

    g_assert_false(app_data.startup_deferred_preset_activation);
    g_assert_true(app_data.startup_waiting_for_preset_switch);
    g_assert_false(app_data.startup_final_content_active);
}

static void
test_confirm_activates_final_content(void)
{
    AppData app_data = {0};

    app_data.startup_waiting_for_preset_switch = true;
    app_data.startup_final_content_active = false;

    startup_gate_confirm_preset_activation(&app_data);

    g_assert_false(app_data.startup_waiting_for_preset_switch);
    g_assert_true(app_data.startup_final_content_active);
}

static void
test_confirm_without_pending_switch_is_noop(void)
{
    AppData app_data = {0};

    app_data.startup_waiting_for_preset_switch = false;
    app_data.startup_final_content_active = false;

    startup_gate_confirm_preset_activation(&app_data);

    g_assert_false(app_data.startup_waiting_for_preset_switch);
    g_assert_false(app_data.startup_final_content_active);
}

int
main(int argc, char** argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/startup-activation/request-keeps-final-inactive",
                    test_request_keeps_final_content_inactive_until_confirmed);
    g_test_add_func("/startup-activation/confirm-activates-final",
                    test_confirm_activates_final_content);
    g_test_add_func("/startup-activation/confirm-noop-without-pending",
                    test_confirm_without_pending_switch_is_noop);

    return g_test_run();
}
