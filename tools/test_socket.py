import socket
import sys

def send_cmd(cmd):
    sock_path = "/run/user/1000/milkdrop-0.sock"
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock_path)
        s.sendall((cmd + "\n").encode())
        response = s.recv(1024)
        print(f"Sent: {cmd} -> Received: {response.decode().strip()}")
        s.close()
    except Exception as e:
        print(f"Error sending {cmd}: {e}")

if __name__ == "__main__":
    send_cmd("preset-dir /home/mauriciobc/presets")
    send_cmd("next")
