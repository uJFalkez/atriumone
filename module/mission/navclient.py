"""Client for navd's control socket (abstract Unix socket "@payload-navd").

Also usable from the shell:  python3 mission/navclient.py state|mag|"reset ID"|stop|quit
"""
import json
import os
import socket
import sys

SOCK = "\0" + os.environ.get("ATRIUM_NAVD_SOCK", "payload-navd")


class NavError(Exception):
    pass


def call(cmd, timeout=6.0):
    """Sends one command, returns the decoded JSON reply. `reset` blocks ~3.5 s in navd."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(SOCK)
        s.sendall(cmd.encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    except OSError as e:
        raise NavError(f"navd unreachable: {e}") from e
    finally:
        s.close()
    if not buf:
        raise NavError("navd closed the connection without a reply")
    return json.loads(buf)


def state(timeout=2.0):
    return call("state", timeout=timeout)


def mag_burst():
    return call("mag", timeout=3.0)


def reset(mission_id):
    return call(f"reset {mission_id}", timeout=10.0)


def resume(mission_id, north, east, height):
    return call(f"resume {mission_id} {north:.3f} {east:.3f} {height:.3f}", timeout=10.0)


def stop():
    return call("stop")


if __name__ == "__main__":
    print(json.dumps(call(" ".join(sys.argv[1:]) or "state"), indent=2))
