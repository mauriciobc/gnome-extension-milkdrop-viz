import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();

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

function _sendOnce(command, path, onError) {
    const data = TEXT_ENCODER.encode(`${command}\n`);
    _connectUnixSocket(path, (connection, error) => {
        if (error) {
            onError(error);
            return;
        }
        _writeAllAndClose(connection, data, writeError => {
            if (writeError)
                onError(writeError);
        });
    });
}

export function sendMilkdropControlCommand(command, socketPath = null) {
    const path = socketPath ?? getMilkdropSocketPath(0);
    _sendOnce(command, path, err => {
        log(`[milkdrop] control failed (${command}): ${err.message}`);
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

    const attempt = (n) => {
        _sendOnce(command, path, err => {
            if (n < maxRetries) {
                GLib.timeout_add(GLib.PRIORITY_DEFAULT, retryDelayMs, () => {
                    attempt(n + 1);
                    return GLib.SOURCE_REMOVE;
                });
            } else {
                log(`[milkdrop] control connect failed after ${maxRetries} retries (${command}): ${err.message}`);
            }
        });
    };

    attempt(0);
}

/**
 * Send multiple control commands in order, spacing dispatches so the renderer can
 * accept each connection (control thread polls ~200ms; parallel connects race).
 * @param {string[]} commands
 * @param {string|null} socketPath
 * @param {number} interCommandDelayMs delay between starting each send (default 120)
 */
export function sendMilkdropControlCommandsSequentially(
    commands,
    socketPath = null,
    interCommandDelayMs = 120,
    maxRetries = 5,
    retryDelayMs = 200
) {
    if (!commands.length)
        return;

    const path = socketPath ?? getMilkdropSocketPath(0);
    let index = 0;

    const sendOne = () => {
        sendMilkdropControlCommandWithRetry(
            commands[index],
            path,
            maxRetries,
            retryDelayMs
        );
        index++;
        if (index < commands.length) {
            GLib.timeout_add(GLib.PRIORITY_DEFAULT, interCommandDelayMs, () => {
                sendOne();
                return GLib.SOURCE_REMOVE;
            });
        }
    };

    sendOne();
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
                input.read_bytes_async(4096, GLib.PRIORITY_DEFAULT, null, (_in, readRes) => {
                    let bytes;
                    try {
                        bytes = _in.read_bytes_finish(readRes);
                    } catch (_e) {
                        closeAndResolve(null);
                        return;
                    }

                    const text = TEXT_DECODER.decode(bytes.get_data());
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
