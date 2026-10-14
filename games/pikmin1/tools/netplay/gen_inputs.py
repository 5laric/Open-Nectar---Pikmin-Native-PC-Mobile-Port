"""Seeded scripted input generator for the netplay determinism harness.

Writes the PKNI binary format (see pc_port/netplay/pc_input_log.h):
10-byte header (magic, version, pad count, record size) followed by one
record per tick, 4 pads each.

v2 (default): 56-byte records, 4 pads of 14 bytes each (11 PADStatus bytes
+ control-yaw u16 LE + flags u8). Each pad carries a plausible slowly
varying control yaw, so replays exercise the yaw-as-input path.
v1 (--v1): 44-byte records, 4 pads of 11 bytes (no yaw).

The generator plays a plausible field session on pad 0: smooth main-stick
walk patterns, periodic A (throw/pluck), B holds (whistle), rare X
(disband), C-stick nudges and trigger presses. It never presses Start or Y
and never opens menus. Pads 1-3 are neutral (no controller).
"""

import argparse
import math
import random
import struct
import sys
from pathlib import Path

MAGIC = b"PKNI"
VERSION_V2 = 2
VERSION_V1 = 1
PAD_COUNT = 4
RECORD_SIZE_V2 = 56
RECORD_SIZE_V1 = 44

# PAD buttons (include/Dolphin/pad.h). Start and Y are never emitted.
BTN_LEFT = 0x0001
BTN_RIGHT = 0x0002
BTN_DOWN = 0x0004
BTN_UP = 0x0008
TRIG_Z = 0x0010
TRIG_R = 0x0020
TRIG_L = 0x0040
BTN_A = 0x0100
BTN_B = 0x0200
BTN_X = 0x0400

STICK_MAX = 96  # comfortable deflection, inside the s8 range


class _PadScript:
    """Plausible field-session script for one pad (walk + buttons)."""

    def __init__(self, rng: random.Random):
        self.rng = rng
        self.wx, self.wy = 0, 0
        self.leg_left = 0
        self.holds = {}
        self.next_a = rng.randint(20, 60)
        self.next_b = rng.randint(80, 200)
        self.next_x = rng.randint(300, 700)
        self.next_c = rng.randint(30, 120)
        self.next_t = rng.randint(100, 300)
        self.cx, self.cy = 0, 0
        self.c_left = 0
        self.trig_l, self.trig_r = 0, 0
        self.t_left = 0

    def tick(self):
        rng = self.rng
        if self.leg_left <= 0:
            # New walk leg: mostly mid deflections, sometimes idle.
            if rng.random() < 0.15:
                self.wx, self.wy = 0, 0
            else:
                ang = rng.uniform(0, 2 * 3.141592653589793)
                mag = rng.randint(40, STICK_MAX)
                self.wx = int(round(mag * math.cos(ang)))
                self.wy = int(round(mag * math.sin(ang)))
            self.leg_left = rng.randint(20, 90)

        # Jitter the stick a little so consecutive ticks are not identical.
        jx = max(-127, min(127, self.wx + rng.randint(-6, 6)))
        jy = max(-127, min(127, self.wy + rng.randint(-6, 6)))
        self.leg_left -= 1

        buttons = 0
        # A presses: short 2-3 tick taps.
        self.next_a -= 1
        if self.next_a <= 0:
            self.holds[BTN_A] = rng.randint(2, 3)
            self.next_a = rng.randint(40, 120)
        # B holds: whistle, 10-30 ticks.
        self.next_b -= 1
        if self.next_b <= 0:
            self.holds[BTN_B] = rng.randint(10, 30)
            self.next_b = rng.randint(120, 300)
        # X: rare single-tick disband.
        self.next_x -= 1
        if self.next_x <= 0:
            self.holds[BTN_X] = 1
            self.next_x = rng.randint(400, 900)
        for btn, left in list(self.holds.items()):
            if left > 0:
                buttons |= btn
                self.holds[btn] = left - 1

        # C-stick nudges.
        self.next_c -= 1
        if self.next_c <= 0:
            self.cx = rng.randint(-80, 80)
            self.cy = rng.randint(-80, 80)
            self.c_left = rng.randint(3, 10)
            self.next_c = rng.randint(50, 200)
        if self.c_left > 0:
            self.c_left -= 1
        else:
            self.cx, self.cy = 0, 0

        # Triggers: occasional analog squeezes plus digital bit.
        self.next_t -= 1
        if self.next_t <= 0:
            self.trig_l = rng.randint(60, 200)
            self.trig_r = rng.randint(60, 200)
            self.t_left = rng.randint(4, 12)
            self.next_t = rng.randint(150, 400)
        if self.t_left > 0:
            self.t_left -= 1
            if self.trig_l > 120:
                buttons |= TRIG_L
            if self.trig_r > 120:
                buttons |= TRIG_R
        else:
            self.trig_l, self.trig_r = 0, 0

        assert not buttons & 0x1800, "Start/Y must never be pressed"
        return (buttons, jx, jy, self.cx, self.cy, self.trig_l, self.trig_r)


def gen_ticks(nticks: int, seed: int, players: int = 1):
    rng = random.Random(seed)
    # Yaw parameters come from a SEPARATE generator so the PAD stream (rng)
    # stays the same with or without yaw: --v1 output for a fixed seed
    # matches the v2 pad bytes.
    yaw_rng = random.Random(seed * 1000003 + 0x59415701)
    # Pad 1 (co-op partner) draws from its own generator so pad 0's stream is
    # byte-identical whether --players is 1 or 2.
    rng2 = random.Random(seed * 1000033 + 0x50320001)
    pad0 = _PadScript(rng)
    pad1 = _PadScript(rng2)
    # Slowly varying control yaw per pad (turns): a base drift plus a
    # gentle sinusoid, so consecutive ticks differ by a fraction of a
    # degree like a real camera, plus per-pad offsets so pads disagree.
    yaw_base = [yaw_rng.uniform(0.0, 1.0) for _ in range(4)]
    yaw_rate = [yaw_rng.uniform(-0.0006, 0.0006) for _ in range(4)]
    yaw_amp = [yaw_rng.uniform(0.0, 0.004) for _ in range(4)]
    yaw_period = [yaw_rng.uniform(300.0, 1200.0) for _ in range(4)]

    for tick in range(nticks):
        b0 = pad0.tick()
        b1 = pad1.tick() if players >= 2 else None
        yaws = []
        for p in range(4):
            turns = (
                yaw_base[p]
                + yaw_rate[p] * tick
                + yaw_amp[p] * math.sin(2 * math.pi * tick / yaw_period[p])
            ) % 1.0
            yaws.append(int(turns * 65536.0 + 0.5) & 0xFFFF)
        yield (b0, b1, yaws)


def _pack_pad(buttons, sx, sy, cx, cy, tl, tr, connected, yaw, v1: bool):
    if v1:
        return struct.pack(
            "<HbbbbBBBBb", buttons, sx, sy, cx, cy, tl, tr, 0, 0, connected
        )
    return struct.pack(
        "<HbbbbBBBBbHb", buttons, sx, sy, cx, cy, tl, tr, 0, 0, connected,
        yaw, 0,
    )


def write_file(path: Path, ticks, v1: bool = False):
    version = VERSION_V1 if v1 else VERSION_V2
    record_size = RECORD_SIZE_V1 if v1 else RECORD_SIZE_V2
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<HHH", version, PAD_COUNT, record_size))
        for b0, b1, yaws in ticks:
            # Pad 0: live input, connected.
            buttons, sx, sy, cx, cy, tl, tr = b0
            f.write(_pack_pad(buttons, sx, sy, cx, cy, tl, tr, 0, yaws[0], v1))
            # Pad 1: scripted partner when --players 2, else neutral.
            if b1 is not None:
                buttons, sx, sy, cx, cy, tl, tr = b1
                f.write(_pack_pad(buttons, sx, sy, cx, cy, tl, tr, 0, yaws[1], v1))
            else:
                f.write(_pack_pad(0, 0, 0, 0, 0, 0, 0, -1, yaws[1], v1))
            # Pads 2-3: neutral, no controller (yaw still varies for realism).
            for p in range(2, 4):
                f.write(_pack_pad(0, 0, 0, 0, 0, 0, 0, -1, yaws[p], v1))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ticks", type=int, required=True)
    p.add_argument("--seed", type=int, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--v1", action="store_true",
                   help="write the v1 format (no yaw); default is v2 with yaw")
    p.add_argument("--players", type=int, default=1, choices=(1, 2),
                   help="script pad 1 as a co-op partner when 2 (default: 1)")
    a = p.parse_args(argv)
    if a.ticks <= 0:
        print("gen_inputs: --ticks must be positive", file=sys.stderr)
        return 2
    write_file(a.out, gen_ticks(a.ticks, a.seed, players=a.players), v1=a.v1)
    print(f"gen_inputs: wrote {a.ticks} ticks (seed {a.seed}) v{'1' if a.v1 else '2'} "
          f"players={a.players} to {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
