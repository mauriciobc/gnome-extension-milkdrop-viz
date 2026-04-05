import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();

export function getMilkdropSocketPath(monitorIndex = 0) {
    const runtimeDir = GLib.get_user_runtime_dir();
    if (!runtimeDir)
        return `/tmp/milkdrop-${monitorIndex}.sock`;
    return `${runtimeDir}/milkdrop-${monitorIndex}.sock`;
}

export function sendMilkdropControlCommand(command, socketPath = null) {
    const path = socketPath ?? getMilkdropSocketPath(0);
    const socketClient = new Gio.SocketClient();
    const socketAddress = Gio.UnixSocketAddress.new(path);
    const data = TEXT_ENCODER.encode(`${command}\n`);

    socketClient.connect_async(socketAddress, null, (client, connectRes) => {
        let connection;
        try {
            connection = client.connect_finish(connectRes);
        } catch (error) {
            log(`[milkdrop] control connect failed (${command}): ${error.message}`);
            return;
        }

        const output = connection.get_output_stream();
        output.write_all_async(data, GLib.PRIORITY_DEFAULT, null, (stream, writeRes) => {
            try {
                stream.write_all_finish(writeRes);
            } catch (error) {
                log(`[milkdrop] control write failed (${command}): ${error.message}`);
            }

            connection.close_async(GLib.PRIORITY_DEFAULT, null, (_conn, closeRes) => {
                try {
                    _conn.close_finish(closeRes);
                } catch (_e) {
                    // Ignore close errors — the command was already written.
                }
            });
        });
    });
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
        const socketClient = new Gio.SocketClient();
        const socketAddress = Gio.UnixSocketAddress.new(socketPath_);
        const request = TEXT_ENCODER.encode('status\n');

        socketClient.connect_async(socketAddress, null, (client, connectRes) => {
            let connection;
            try {
                connection = client.connect_finish(connectRes);
            } catch (_e) {
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
                    for (const line of text.split('\n')) {
                        if (line === '')
                            break;
                        const eqIdx = line.indexOf('=');
                        if (eqIdx === -1)
                            continue;
                        const key = line.substring(0, eqIdx);
                        const value = line.substring(eqIdx + 1);
                        switch (key) {
                        case 'fps':        status.fps = parseFloat(value); break;
                        case 'paused':     status.paused = value === '1'; break;
                        case 'preset':     status.preset = value; break;
                        case 'audio':      status.audio = value; break;
                        case 'quarantine': status.quarantine = parseInt(value, 10); break;
                        }
                    }
                    closeAndResolve(Object.keys(status).length > 0 ? status : null);
                });
            });
        });
    });
}
