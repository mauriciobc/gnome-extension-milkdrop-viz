export const RETRY_DELAYS_MS = [1000, 2000, 5000, 10000];

export const PAUSE_REASON_FULLSCREEN = 'fullscreen';
export const PAUSE_REASON_MAXIMIZED = 'maximized';
export const PAUSE_REASON_MPRIS = 'mpris';

export const FADE_DURATION_MS = 1000;

// Debounce delay for background reload after monitors-changed signal.
// Prevents clone thrashing when multiple signals fire in rapid succession.
export const RELOAD_BACKGROUNDS_DEBOUNCE_MS = 100;

// Control socket retry settings for initial pause command.
// The renderer needs time to initialize before the socket is available.
export const CONTROL_SOCKET_RETRY_DELAY_MS = 200;
export const CONTROL_SOCKET_MAX_RETRIES = 5;

// Second force_exit() after this delay if the renderer subprocess is still shutting down.
// Keeps a single wait_async in _spawnProcess; escalation does not add another wait.
export const RENDERER_STOP_ESCALATION_MS = 800;
