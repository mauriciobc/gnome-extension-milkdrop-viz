#include "startup_gate.h"

void
startup_gate_request_preset_activation(AppData* app_data)
{
    if (!app_data)
        return;

    app_data->startup_deferred_preset_activation = false;
    app_data->startup_waiting_for_preset_switch = true;
}

void
startup_gate_confirm_preset_activation(AppData* app_data)
{
    if (!app_data || !app_data->startup_waiting_for_preset_switch)
        return;

    app_data->startup_waiting_for_preset_switch = false;
    app_data->startup_final_content_active = true;
}
