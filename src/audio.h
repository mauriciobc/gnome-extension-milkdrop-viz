#pragma once

#include "app.h"

#include <stddef.h>

/* Drop trailing float if count is odd so L/R pairs stay aligned (PipeWire path). */
size_t audio_align_stereo_float_count(size_t sample_count);

bool audio_init(AppData* app_data);
void audio_cleanup(AppData* app_data);