#!/usr/bin/env python3
"""Read-only console/WiThrottle acceptance against an already-loaded station."""
import argparse
import socket
import time
from contextlib import ExitStack


def receive(client, expected=None, seconds=5):
    data = bytearray()
    deadline = time.monotonic() + seconds
    client.settimeout(0.2)
    while time.monotonic() < deadline:
        try:
            chunk = client.recv(4096)
            if not chunk:
                raise RuntimeError("Station closed the connection")
            data.extend(chunk)
            if expected is not None and expected in data:
                return bytes(data)
        except socket.timeout:
            pass
    if expected is not None:
        raise RuntimeError(f"Missing {expected!r}; received {bytes(data)!r}")
    return bytes(data)


def connect(host, port):
    return socket.create_connection((host, port), timeout=4)


def open_console(host, port):
    client = connect(host, port)
    try:
        # Send nothing first: reproduce clients waiting for the server to start.
        offer = receive(client, bytes([255, 251, 1]))
        if b"enter #console" in offer:
            raise RuntimeError("Server sent application text before protocol identification")
        client.sendall(bytes([255, 253, 1, 255, 253, 3, 255, 251, 3]))
        receive(client, b"enter #console")
        client.sendall(b"#console\r\n")
        greeting = receive(client, b"dcc> ")
        if b"DCC-EX console" not in greeting:
            raise RuntimeError(f"Unexpected greeting: {greeting!r}")
        # Repeat echo acknowledgement bytewise: it must not start a reply loop.
        for value in (255, 253, 1):
            client.sendall(bytes([value]))
            time.sleep(0.05)
        return client
    except Exception:
        client.close()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="IP displayed by the station")
    parser.add_argument("--port", type=int, default=2560)
    args = parser.parse_args()
    with ExitStack() as stack:
        console = stack.enter_context(open_console(args.host, args.port))
        console.sendall(b"<s>\r\n")
        status = receive(console, b"<iDCC-EX")
        status += receive(console, seconds=0.3)
        if b"<s>\r\n" not in status or b"\r\r\n" in status:
            raise RuntimeError(f"Console echo/line ending failure: {status!r}")
        print("PASS: console status", status.decode("ascii", "backslashreplace").strip())

        throttle = stack.enter_context(connect(args.host, args.port))
        # Client identification/metadata only: no loco acquisition or power commands.
        throttle.sendall(b"Ncodex-readonly\n")
        intro = receive(throttle, b"VN2.0")
        intro += receive(throttle, seconds=0.3)
        if b"dcc>" in intro or b"<*" in intro:
            raise RuntimeError("Console output leaked into the WiThrottle connection")
        print("PASS: simultaneous WiThrottle protocol handshake")

        console.sendall(b"diag on\r\n")
        receive(console, b"Diagnostics on")
        receive(console, seconds=0.3)
        console.sendall(b"<D RAM>\r\n")
        diagnostic = receive(console, b"Free memory=")
        diagnostic += receive(console, seconds=0.3)
        print("PASS: remote diagnostic", diagnostic.decode("ascii", "backslashreplace").strip())
        leaked = receive(throttle, seconds=0.3)
        if b"Free memory=" in leaked or b"dcc>" in leaked or b"<*" in leaked:
            raise RuntimeError("Diagnostics leaked into the WiThrottle connection")
        print("PASS: diagnostic isolation")

        # Leave subscription enabled to verify it is reset on close/reuse.
        console.close()
        time.sleep(0.3)
        restored = stack.enter_context(open_console(args.host, args.port))
        restored.sendall(b"<D RAM>\r\n")
        output = receive(restored, b"dcc> ")
        output += receive(restored, seconds=0.5)
        if b"Free memory=" in output:
            raise RuntimeError("Diagnostic subscription survived reconnect")
        print("PASS: console reconnect resets diagnostic subscription")

        throttle.sendall(b"Ncodex-readonly\n")
        output = receive(throttle, b"*")
        output += receive(throttle, seconds=0.3)
        if b"dcc>" in output or b"<*" in output:
            raise RuntimeError("Unexpected console data after reconnect")
        print("PASS: original WiThrottle connection remains responsive")
    print("Network acceptance passed; iPad UI, physical throttle controls and no-USB boot remain separate tests.")


if __name__ == "__main__":
    main()
