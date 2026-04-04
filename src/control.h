#pragma once

#include "app.h"

typedef enum {
	CONTROL_CMD_NONE = 0,
	CONTROL_CMD_STATUS,
	CONTROL_CMD_OPACITY,
	CONTROL_CMD_PAUSE,
	CONTROL_CMD_SHUFFLE,
	CONTROL_CMD_OVERLAY,
	CONTROL_CMD_PRESET_DIR,
} ControlCommandType;

typedef enum {
	CONTROL_PARSE_OK = 0,
	CONTROL_PARSE_INCOMPLETE,
	CONTROL_PARSE_INVALID,
	CONTROL_PARSE_PATH_TOO_LONG,
} ControlParseResult;

typedef struct {
	ControlCommandType type;
	float opacity;
	bool pause_enabled;
	bool bool_value;
	char text_value[MILKDROP_PATH_MAX];
} ControlCommand;

ControlParseResult control_parse_command(const char* line, ControlCommand* out_command);

bool control_init(AppData* app_data);
void control_cleanup(AppData* app_data);