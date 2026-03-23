#!/usr/bin/env python3
"""Send commands to MudProxy via Unix socket."""
import socket
import json
import sys
import time

SOCK = "/tmp/mudproxy_cmd.sock"

def send(action: str, **kwargs) -> dict:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    req = {"action": action, **kwargs}
    s.sendall(json.dumps(req).encode() + b'\n')
    resp = json.loads(s.recv(8192).decode())
    s.close()
    return resp

def cmd(text: str) -> dict:
    return send("command", text=text)

def status() -> dict:
    return send("status")

def grind_start() -> dict:
    return send("grind_start")

def grind_stop() -> dict:
    return send("grind_stop")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        text = " ".join(sys.argv[1:])
        if text == "grind":
            print(grind_start())
        elif text == "grind stop":
            print(grind_stop())
        else:
            print(cmd(text))
    else:
        print(status())
