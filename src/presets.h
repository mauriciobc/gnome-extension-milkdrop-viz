#pragma once

#include <stdbool.h>

#include "app.h"

bool presets_reload(AppData* app_data);
void presets_clear(AppData* app_data);
const char* presets_current(const AppData* app_data);
