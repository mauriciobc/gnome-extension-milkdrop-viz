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
	CONTROL_CMD_NEXT_PRESET,
	CONTROL_CMD_PREV_PRESET,
	CONTROL_CMD_FPS,
	CONTROL_CMD_ROTATION_INTERVAL,
	CONTROL_CMD_SAVE_STATE,
	CONTROL_CMD_RESTORE_STATE,
} ControlCommandType;

typedef enum {
	CONTROL_PARSE_OK = 0,
	CONTROL_PARSE_INCOMPLETE,
	CONTROL_PARSE_INVALID,
	CONTROL_PARSE_PATH_TOO_LONG,
} ControlParseResult;

typedef enum {
	CONTROL_AUDIO_OK = 0,
	CONTROL_AUDIO_RECOVERING,
	CONTROL_AUDIO_FAILED,
} ControlAudioStatus;

typedef struct {
	ControlCommandType type;
	float opacity;
	bool pause_enabled;
	bool bool_value;
	int  int_value;
	char text_value[MILKDROP_PATH_MAX];
} ControlCommand;

typedef struct {
	float              fps;
	bool               paused;
	char               preset[MILKDROP_PATH_MAX];
	ControlAudioStatus audio;
	int                quarantine_count;
} StatusResponse;

ControlParseResult control_parse_command(const char* line, ControlCommand* out_command);
bool               status_response_parse(const char* response, StatusResponse* out);

/**
 * Returns a heap-allocated socket path for the given monitor index,
 * e.g. /run/user/1000/milkdrop-0.sock. Caller must g_free() the result.
 */
gchar* control_socket_path_for_monitor(int monitor_index);

bool control_init(AppData* app_data);
void control_cleanup(AppData* app_data);