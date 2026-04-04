import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

export function getMilkdropSocketPath() {
    const runtimeDir = GLib.get_user_runtime_dir();
    if (!runtimeDir)
        return '/tmp/milkdrop.sock';
    return `${runtimeDir}/milkdrop.sock`;
}

export function sendMilkdropControlCommand(command) {
    const socketPath = getMilkdropSocketPath();
    const socketClient = new Gio.SocketClient();
    const socketAddress = Gio.UnixSocketAddress.new(socketPath);
    const data = new TextEncoder().encode(`${command}\n`);

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
