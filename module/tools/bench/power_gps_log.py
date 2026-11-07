#!/usr/bin/env python3
"""Once per second, append power/thermal state, GPS status and new kernel messages to a log on
disk, fsync'd, so the log survives a sudden reset. Used to debug the reset seen with the GPS powered.

usage: power_gps_log.py [logfile=logs/power_gps.log] [uart=/dev/ttyS0]
"""
import os, sys, time, termios, subprocess

LOG = sys.argv[1] if len(sys.argv) > 1 else "logs/power_gps.log"
UART = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyS0"
os.makedirs(os.path.dirname(LOG) or ".", exist_ok=True)
log = open(LOG, "a")


def emit(line):
    log.write(line + "\n")
    log.flush()
    os.fsync(log.fileno())


def vc(*args):
    try:
        return subprocess.run(["vcgencmd", *args], capture_output=True, text=True, timeout=2).stdout.strip().split("=")[-1]
    except Exception as e:
        return f"?{e.__class__.__name__}"


def open_uart():
    fd = os.open(UART, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
    a[4] = a[5] = termios.B9600
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


fd = open_uart()
dm = subprocess.Popen(["dmesg", "-w", "--since", "now"], stdout=subprocess.PIPE, text=True) \
    if subprocess.run(["dmesg", "--help"], capture_output=True, text=True).stdout.find("--since") >= 0 \
    else subprocess.Popen(["dmesg", "-w", "-T"], stdout=subprocess.PIPE, text=True)
os.set_blocking(dm.stdout.fileno(), False)
emit(f"=== start {time.strftime('%Y-%m-%d %H:%M:%S')} uptime={open('/proc/uptime').read().split()[0]}s")

buf = b""
fix = sats = "-"
skip_dmesg_backlog = time.monotonic() + 1.5   # old dmesg -w prints the whole ring first
while True:
    t_end = time.monotonic() + 1.0
    nbytes = 0
    while time.monotonic() < t_end:
        try:
            d = os.read(fd, 4096)
            nbytes += len(d); buf += d
        except BlockingIOError:
            time.sleep(0.05)
    *lines, buf = buf.split(b"\n")
    for ln in lines:
        if ln.startswith(b"$GPGGA") or ln.startswith(b"$GNGGA"):
            f = ln.decode("ascii", "replace").split(",")
            if len(f) > 7:
                fix, sats = f[6] or "-", f[7] or "-"
    kmsgs = []
    try:
        for kl in dm.stdout:
            if time.monotonic() > skip_dmesg_backlog:
                kmsgs.append(kl.strip())
    except (BlockingIOError, TypeError):
        pass
    up = open("/proc/uptime").read().split()[0]
    emit(f"{time.strftime('%H:%M:%S')} up={up} throttled={vc('get_throttled')} core={vc('measure_volts', 'core')} "
         f"temp={vc('measure_temp')} gps_bytes={nbytes} fix={fix} sats={sats}")
    for k in kmsgs:
        emit("  kmsg: " + k)
