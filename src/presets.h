#pragma once

#include <stdbool.h>

#include "app.h"

bool presets_reload(AppData* app_data);
void presets_clear(AppData* app_data);
const char* presets_current(const AppData* app_data);
bool presets_apply_dir_change(AppData* app_data, const char* next_dir);

void presets_start_async_scan(AppData* app_data);
void presets_cleanup_async(AppData* app_data);

