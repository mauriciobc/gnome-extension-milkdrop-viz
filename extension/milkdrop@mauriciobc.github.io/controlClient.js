import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();

/** Match MILKDROP_PATH_MAX / MILKDROP_CONTROL_RECV_MAX in src/control.h */
const MILKDROP_PATH_MAX = 4096;
/** Align with MILKDROP_CONTROL_RECV_MAX in src/control.h (PATH_MAX * 5). */
const CONTROL_MAX_RESPONSE_BYTES = MILKDROP_PATH_MAX * 5;

function _connectUnixSocket(path, callback) {
    const socketClient = new Gio.SocketClient();
    const socketAddress = Gio.UnixSocketAddress.new(path);
    socketClient.connect_async(socketAddress, null, (client, connectRes) => {
        let connection;
        try {
            connection = client.connect_finish(connectRes);
        } catch (error) {
            callback(null, error);
            return;
        }
        callback(connection, null);
    });
}

/**
 * Read until EOF or maxBytes, concatenating chunks (Unix stream closes after one response).
 */
function _readResponseFully(input, maxBytes, callback) {
    let totalSoFar = 0;
    const parts = [];

    const readChunk = () => {
        if (totalSoFar >= maxBytes) {
            callback(null);
            return;
        }

        const ask = Math.min(8192, maxBytes - totalSoFar);
        input.read_bytes_async(ask, GLib.PRIORITY_DEFAULT, null, (_in, readRes) => {
            let bytes;
            try {
                bytes = _in.read_bytes_finish(readRes);
            } catch (_e) {
                callback(null);
                return;
            }

            const n = bytes.get_size();
            if (n === 0) {
                const buf = new Uint8Array(totalSoFar);
                let o = 0;
                for (const p of parts) {
                    buf.set(p, o);
                    o += p.length;
                }
                callback(buf);
                return;
            }

            const data = bytes.get_data();
            const arr = new Uint8Array(n);
            for (let i = 0; i < n; i++)
                arr[i] = data[i];

            parts.push(arr);
            totalSoFar += n;
            readChunk();
        });
    };

    readChunk();
}

function _writeAllAndClose(connection, data, callback) {
    const output = connection.get_output_stream();
    output.write_all_async(data, GLib.PRIORITY_DEFAULT, null, (stream, writeRes) => {
        let writeError = null;
        try {
            stream.write_all_finish(writeRes);
        } catch (error) {
            writeError = error;
        }

        connection.close_async(GLib.PRIORITY_DEFAULT, null, (_conn, closeRes) => {
            try {
                _conn.close_finish(closeRes);
            } catch (_e) {
                // Ignore close errors — command was already attempted.
            }
            callback(writeError);
        });
    });
}

export function getMilkdropSocketPath(monitorIndex = 0) {
    const runtimeDir = GLib.get_user_runtime_dir();
    if (!runtimeDir)
        return `/tmp/milkdrop-${monitorIndex}.sock`;
    return `${runtimeDir}/milkdrop-${monitorIndex}.sock`;
}

export function sendMilkdropControlCommand(command, socketPath = null) {
    const path = socketPath ?? getMilkdropSocketPath(0);
    const data = TEXT_ENCODER.encode(`${command}\n`);

    _connectUnixSocket(path, (connection, error) => {
        if (error) {
            log(`[milkdrop] control connect failed (${command}): ${error.message}`);
            return;
        }

        _writeAllAndClose(connection, data, writeError => {
            if (writeError)
                log(`[milkdrop] control write failed (${command}): ${writeError.message}`);
        });
    });
}

/**
 * Send a control command with retry logic for socket initialization race.
 * Silently retries on connect failure until maxRetries is reached.
 * Only logs error on final failure.
 *
 * @param {string} command - The command to send
 * @param {string|null} socketPath - Socket path or null for default
 * @param {number} maxRetries - Maximum retry attempts (default 5)
 * @param {number} retryDelayMs - Delay between retries in ms (default 200)
 */
export function sendMilkdropControlCommandWithRetry(
    command,
    socketPath = null,
    maxRetries = 5,
    retryDelayMs = 200
) {
    const path = socketPath ?? getMilkdropSocketPath(0);
    const data = TEXT_ENCODER.encode(`${command}\n`);

    const attemptSend = (attempt) => {
        _connectUnixSocket(path, (connection, error) => {
            if (error) {
                if (attempt < maxRetries) {
                    // Silent retry after delay
                    GLib.timeout_add(GLib.PRIORITY_DEFAULT, retryDelayMs, () => {
                        attemptSend(attempt + 1);
                        return GLib.SOURCE_REMOVE;
                    });
                } else {
                    // Final failure - log the error
                    log(`[milkdrop] control connect failed after ${maxRetries} retries (${command}): ${error.message}`);
                }
                return;
            }

            _writeAllAndClose(connection, data, writeError => {
                if (writeError)
                    log(`[milkdrop] control write failed (${command}): ${writeError.message}`);
            });
        });
    };

    attemptSend(1);
}

/**
 * Query the renderer status over the control socket.
 * Returns a Promise that resolves to an object with keys:
 *   fps (number), paused (bool), preset (string), audio (string), quarantine (number)
 * or null if the renderer is not running / connection failed.
 */
export function queryMilkdropStatus(socketPath = null) {
    return new Promise(resolve => {
        const socketPath_ = socketPath ?? getMilkdropSocketPath(0);
        const request = TEXT_ENCODER.encode('status\n');

        _connectUnixSocket(socketPath_, (connection, error) => {
            if (error) {
                resolve(null);
                return;
            }

            const closeAndResolve = result => {
                connection.close_async(GLib.PRIORITY_DEFAULT, null, () => {});
                resolve(result);
            };

            const output = connection.get_output_stream();
            output.write_all_async(request, GLib.PRIORITY_DEFAULT, null, (_out, writeRes) => {
                try {
                    _out.write_all_finish(writeRes);
                } catch (_e) {
                    closeAndResolve(null);
                    return;
                }

                const input = connection.get_input_stream();
                _readResponseFully(input, MILKDROP_PATH_MAX * 2 + 512, buf => {
                    if (!buf || buf.length === 0) {
                        closeAndResolve(null);
                        return;
                    }

                    const text = TEXT_DECODER.decode(buf);
                    const status = {};

                    const parseByKey = {
                        fps: v => { status.fps = parseFloat(v); },
                        paused: v => { status.paused = v === '1'; },
                        preset: v => { status.preset = v; },
                        audio: v => { status.audio = v; },
                        quarantine: v => { status.quarantine = parseInt(v, 10); },
                    };

                    for (const line of text.split('\n')) {
                        if (line === '')
                            break;
                        const eqIdx = line.indexOf('=');
                        if (eqIdx === -1)
                            continue;
                        const key = line.substring(0, eqIdx);
                        const value = line.substring(eqIdx + 1);
                        const parse = parseByKey[key];
                        if (parse)
                            parse(value);
                    }

                    closeAndResolve(Object.keys(status).length > 0 ? status : null);
                });
            });
        });
    });
}

/**
 * Request a one-line state snapshot from the renderer (`save-state`).
 * Returns the line without trailing newline, or null if the renderer is down / error response.
 * Do not log the returned string at info level — it may be long.
 */
export function queryMilkdropSaveState(socketPath = null) {
    return new Promise(resolve => {
        const socketPath_ = socketPath ?? getMilkdropSocketPath(0);
        const request = TEXT_ENCODER.encode('save-state\n');

        _connectUnixSocket(socketPath_, (connection, error) => {
            if (error) {
                resolve(null);
                return;
            }

            const closeAndResolve = result => {
                connection.close_async(GLib.PRIORITY_DEFAULT, null, () => {});
                resolve(result);
            };

            const output = connection.get_output_stream();
            output.write_all_async(request, GLib.PRIORITY_DEFAULT, null, (_out, writeRes) => {
                try {
                    _out.write_all_finish(writeRes);
                } catch (_e) {
                    closeAndResolve(null);
                    return;
                }

                const input = connection.get_input_stream();
                _readResponseFully(input, CONTROL_MAX_RESPONSE_BYTES, buf => {
                    if (!buf || buf.length === 0) {
                        closeAndResolve(null);
                        return;
                    }

                    const text = TEXT_DECODER.decode(buf).trimEnd();
                    const firstLine = text.split('\n')[0] ?? '';

                    if (firstLine.startsWith('err '))
                        closeAndResolve(null);
                    else
                        closeAndResolve(firstLine);
                });
            });
        });
    });
}

/**
 * Apply a snapshot line previously returned by {@link queryMilkdropSaveState}.
 * Fire-and-forget; same semantics as {@link sendMilkdropControlCommand}.
 */
export function sendMilkdropRestoreState(savedStateLine, socketPath = null) {
    const line = (savedStateLine ?? '').trim();
    const command = line.length === 0 ? 'restore-state' : `restore-state ${line}`;
    sendMilkdropControlCommand(command, socketPath);
}

/**
 * Snapshot state from all renderer instances (one per monitor).
 * Resolves to an array of { monitorIndex, saveState } where saveState is string or null.
 */
export function queryAllMilkdropSaveState(numMonitors) {
    const promises = [];
    for (let i = 0; i < numMonitors; i++) {
        const socketPath = getMilkdropSocketPath(i);
        promises.push(queryMilkdropSaveState(socketPath).then(saveState => ({
            monitorIndex: i,
            saveState,
        })));
    }
    return Promise.all(promises);
}

/**
 * Query status from all running renderer instances (one per monitor).
 * Returns a Promise that resolves to an array of status objects,
 * or an empty array if no renderers are running.
 */
export function queryAllMilkdropStatus(numMonitors) {
    const promises = [];
    for (let i = 0; i < numMonitors; i++) {
        const socketPath = getMilkdropSocketPath(i);
        promises.push(queryMilkdropStatus(socketPath).then(status => ({
            monitorIndex: i,
            status,
        })));
    }
    return Promise.all(promises);
}
