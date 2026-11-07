#!/usr/bin/env python3
"""Bench test: MPU6500 + QMC5883P time-multiplexed on one I2C bus.

The MPU is read continuously at RATE Hz. Every PERIOD s the MPU reads pause and the mag
runs a short normal-mode burst (wake, drop 3, average ~27 samples, suspend), then the
MPU resumes. Reports MPU bad frames, mag burst outcomes, MPU pause length, |B| spread.

usage: tdm_test.py [duration_s=600] [period_s=5] [mpu_rate_hz=50] [bus=1]
"""
import sys, time, struct, statistics as S
from smbus2 import SMBus, i2c_msg

DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 600
PERIOD = float(sys.argv[2]) if len(sys.argv) > 2 else 5
RATE = float(sys.argv[3]) if len(sys.argv) > 3 else 50
BUS = int(sys.argv[4]) if len(sys.argv) > 4 else 1
DT = 1 / RATE
MPU, QMC = 0x68, 0x2C
QMC_LSB_PER_UT = 3750 / 100      # ±8 G range
MAG_CTRL1_NORMAL, MAG_CTRL1_SUSPEND, MAG_CTRL2 = 0x89, 0x88, 0x08

b = SMBus(BUS)
T0 = time.monotonic()
wfail = []


def now():
    return round(time.monotonic() - T0, 1)


def w(addr, reg, val):
    for _ in range(5):
        try:
            b.write_byte_data(addr, reg, val)
            return True
        except OSError:
            time.sleep(0.01)
    wfail.append((hex(addr), now()))
    return False


def rdblk(addr, reg, n):
    wm, r = i2c_msg.write(addr, [reg]), i2c_msg.read(addr, n)
    b.i2c_rdwr(wm, r)
    return bytes(r)


def mag_burst():
    if not w(QMC, 0x0A, MAG_CTRL1_NORMAL):
        return None, "nack"
    s, k, e, t0 = [], 0, 0, time.monotonic()
    while len(s) < 27 and time.monotonic() - t0 < 1.0:
        try:
            if b.read_byte_data(QMC, 0x09) & 1:
                f = [x / QMC_LSB_PER_UT for x in struct.unpack('<3h', rdblk(QMC, 0x01, 6))]
                k += 1
                if k > 3:
                    s.append(f)
        except OSError:
            e += 1
        time.sleep(0.003)
    w(QMC, 0x0A, MAG_CTRL1_SUSPEND)
    try:
        c = rdblk(QMC, 0x0A, 2)
        cfg_ok = c[0] == MAG_CTRL1_SUSPEND and (c[1] & 0x7F) == MAG_CTRL2
    except OSError:
        cfg_ok = False
    if len(s) < 20:
        return None, f"short({len(s)},err{e})"
    med = [S.median(c) for c in zip(*s)]
    if any(x == 0 for x in med):
        return med, "x0"
    return med, "ok" if cfg_ok else "cfg"


# MPU: clock PLL, DLPF ~20 Hz below 100 Hz (anti-alias) else ~41 Hz, ±500 dps, ±4 g
dlpf = 0x03 if RATE > 100 else 0x04
w(MPU, 0x6B, 0x01); time.sleep(0.05)
w(MPU, 0x1A, dlpf); w(MPU, 0x1B, 0x08); w(MPU, 0x1C, 0x08); w(MPU, 0x1D, dlpf)
w(MPU, 0x19, int(round(1000 / RATE)) - 1)
# QMC: reset, dummy CTRL2 (swallowed after reset), axis sign, range, suspend
w(QMC, 0x0B, 0x80); time.sleep(0.05)
w(QMC, 0x0B, 0x00); w(QMC, 0x29, 0x06); w(QMC, 0x0B, MAG_CTRL2); w(QMC, 0x0A, MAG_CTRL1_SUSPEND)

n = bad = err = 0
ex, res, Bs, gaps = [], {}, [], []
t = time.monotonic(); end = t + DUR; next_burst = t + 1; last_report = t
print(f"bus {BUS}, MPU {RATE:.0f} Hz, mag burst every {PERIOD}s, {DUR:.0f}s", flush=True)
while time.monotonic() < end:
    if time.monotonic() >= next_burst:
        g0 = time.monotonic()
        med, st = mag_burst()
        gaps.append(time.monotonic() - g0)
        res[st] = res.get(st, 0) + 1
        if med:
            Bs.append(med)
        if res.get("nack", 0) >= 3:
            print("mag gone at t =", now(), flush=True)
            break
        next_burst = time.monotonic() + PERIOD
        t = time.monotonic()
    try:
        v = struct.unpack('>7h', rdblk(MPU, 0x3B, 14))
        n += 1
        g = [x / 65.5 for x in v[4:]]
        a = [x / 8192 * 9.80665 for x in v[:3]]
        if any(abs(x) > 30 for x in g) or not 9.3 < sum(x * x for x in a) ** .5 < 10.4:
            bad += 1
            if len(ex) < 6:
                ex.append((now(), v))
    except OSError:
        err += 1
    if time.monotonic() - last_report > 60:
        print(f"  t={now():.0f}s mpu={n} bad={bad} oserr={err} mag={res}", flush=True)
        last_report = time.monotonic()
    t += DT
    d = t - time.monotonic()
    if d > 0:
        time.sleep(d)

print(f"t={now():.0f}s MPU reads={n} bad={bad} oserr={err} write_fail={wfail[:10]}")
if gaps:
    print("mag bursts:", res, f"MPU pause {S.mean(gaps) * 1000:.0f} ms avg, {max(gaps) * 1000:.0f} ms max")
if len(Bs) > 1:
    B = [sum(x * x for x in m) ** .5 for m in Bs]
    print(f"|B| {S.mean(B):.2f}±{S.pstdev(B):.2f} uT, sd per axis {[round(S.pstdev(c), 2) for c in zip(*Bs)]}")
for e in ex:
    print("bad MPU frame", e)
