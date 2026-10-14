#!/usr/bin/env python3
"""Two-process run of the host-authoritative online mode (--online-host /
--online-join) on loopback, for development and regression.

The host opens a room with PIKMIN_ONLINE_TEST_LOOPBACK=1 (no router, the code
carries 127.0.0.1) and writes its one code to a file; the guest joins with
that code. Both play scripted input (gen_inputs.py), run in real time and stop
after --ticks. With --lag/--jitter/--loss, GNS degrades every packet each
process sends (NECTAR_NET_TEST_IMPAIR), so --lag 75 gives an RTT of ~150 ms.

Afterwards the captain positions both peers printed every 150 ticks
([netplay-navi], PIKMIN_NETPLAY_DEBUG_NAVI_POS) are compared:
  * the guest's captain (navi 1) on the guest is its corrected prediction and
    must stay close to the host's authoritative position for the same tick;
  * the host's captain (navi 0) on the guest is interpolated in the past, so
    it lags by roughly the delay times the speed; it is reported, not judged.

Reuses run_pair.py for run folders, scripted input and process launch. Only
the two spawned processes are ever killed. Set SDL_VIDEODRIVER=offscreen to
keep the windows off screen.
"""

import argparse
import math
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_pair  # noqa: E402

NAVI_RE = re.compile(r"\[netplay-navi\] tick=(\d+) navi=(\d) pos=\(([-\d.]+) ([-\d.]+) ([-\d.]+)\)")
STATS_RE = re.compile(r"\[online\] tick=(\d+) (.*)")


def navi_positions(log):
    positions = {}
    for match in NAVI_RE.finditer(log):
        tick, navi = int(match.group(1)), int(match.group(2))
        positions[(tick, navi)] = tuple(float(match.group(i)) for i in (3, 4, 5))
    return positions


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def path_length(positions, navi):
    points = [positions[k] for k in sorted(k for k in positions if k[1] == navi)]
    return sum(distance(points[i], points[i + 1]) for i in range(len(points) - 1))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--exe", type=Path, required=True, help="netplay build with NECTAR_ONLINE and GNS")
    p.add_argument("--assets", type=Path, required=True, help="extracted game files (holding dataDir/)")
    p.add_argument("--out", type=Path, required=True, help="run folder (recreated)")
    p.add_argument("--ticks", type=int, default=3000)
    p.add_argument("--port", type=int, default=39911)
    p.add_argument("--lag", type=float, default=0.0, help="one-way lag added by GNS, ms")
    p.add_argument("--jitter", type=float, default=0.0, help="extra random lag up to this many ms")
    p.add_argument("--loss", type=float, default=0.0, help="packet loss per direction, percent")
    p.add_argument("--frames", action="store_true",
                   help="dump every 15th presented frame of each peer to <run>/frames (PIKMIN_FRAME_DUMP)")
    p.add_argument("--host-start-at", default="",
                   help="TEST ONLY: comma-separated host ticks where the host presses Start (pause/unpause)")
    p.add_argument("--guest-leave-at", type=int, default=0, help="TEST ONLY: guest tick to leave co-op")
    p.add_argument("--guest-rejoin-at", type=int, default=0, help="TEST ONLY: guest tick to ask to rejoin")
    p.add_argument("--max-error", type=float, default=25.0,
                   help="fail when the guest captain's mean error exceeds this (world units)")
    a = p.parse_args(argv)

    assets = a.assets.resolve()
    if not (assets / "dataDir").is_dir():
        print(f"run_online_pair: {assets} has no dataDir/")
        return 2
    out = a.out.resolve()
    if out.exists():
        import shutil
        shutil.rmtree(out)
    host_run, join_run = out / "host", out / "join"
    host_pkni = run_pair.prepare_run(host_run, assets, None, a.ticks, 1)
    join_pkni = run_pair.prepare_run(join_run, assets, None, a.ticks, 2)

    impair = {}
    if a.lag or a.jitter or a.loss:
        impair = {"NECTAR_NET_TEST_IMPAIR": "1", "NECTAR_NET_TEST_LAG_MS": str(a.lag),
                  "NECTAR_NET_TEST_JITTER_MS": str(a.jitter), "NECTAR_NET_TEST_LOSS_PCT": str(a.loss)}
    code_file = host_run / "code.txt"
    if a.frames:
        for run in (host_run, join_run):
            (run / "frames").mkdir(exist_ok=True)
        impair = dict(impair, PIKMIN_FRAME_DUMP="frames")  # relative to each peer's run folder
    host_env = dict(impair, PIKMIN_ONLINE_TEST_LOOPBACK="1")
    if a.host_start_at:
        host_env.update(PIKMIN_NETPLAY_TEST_KNOBS="1", PIKMIN_ONLINE_TEST_START_AT=a.host_start_at)
    hproc, hout = run_pair.launch(a.exe, host_run, ["--online-host", str(a.port), "--online-code-out", str(code_file),
                                                    "--netplay-input", str(host_pkni)], host_env, False, a.ticks)
    deadline = time.time() + 60
    while not code_file.exists() and time.time() < deadline and hproc.poll() is None:
        time.sleep(0.1)
    if not code_file.exists():
        print("run_online_pair: the host wrote no code")
        hproc.kill()
        return 1
    join_env = dict(impair)
    if a.guest_leave_at:
        join_env.update(PIKMIN_NETPLAY_TEST_KNOBS="1", PIKMIN_ONLINE_TEST_LEAVE_AT=str(a.guest_leave_at),
                        PIKMIN_ONLINE_TEST_REJOIN_AT=str(a.guest_rejoin_at))
    jproc, jout = run_pair.launch(a.exe, join_run, ["--online-join", "@" + str(code_file), "--netplay-input",
                                                    str(join_pkni)], join_env, False, a.ticks)
    rcs = run_pair.wait_all([("host", hproc), ("join", jproc)], a.ticks / 30.0 + 180)
    hout.close()
    jout.close()

    host_log = (host_run / "native.log").read_text(errors="replace")
    join_log = (join_run / "native.log").read_text(errors="replace")
    for name, log in (("host", host_log), ("join", join_log)):
        print(f"run_online_pair: {name}: exit={rcs.get(name)}")
        for line in log.splitlines():
            if line.startswith("[online]") and any(w in line for w in ("room", "joined", "ended", "left", "rules",
                                                                         "pause", "test:", "spectat", "rejoin",
                                                                         "back in")):
                print(f"run_online_pair: {name}:   {line}")
        stats = STATS_RE.findall(log)
        if stats:
            print(f"run_online_pair: {name}:   last: tick={stats[-1][0]} {stats[-1][1]}")

    host_pos, join_pos = navi_positions(host_log), navi_positions(join_log)
    shared = sorted(k for k in host_pos if k in join_pos)
    failures = []
    for navi, label in ((1, "guest captain (predicted+corrected)"), (0, "host captain (interpolated)")):
        errors = [distance(host_pos[k], join_pos[k]) for k in shared if k[1] == navi]
        if not errors:
            failures.append(f"no shared {label} samples")
            continue
        mean = sum(errors) / len(errors)
        print(f"run_online_pair: {label}: {len(errors)} samples, mean error {mean:.2f}, max {max(errors):.2f}, "
              f"host path {path_length(host_pos, navi):.1f}")
        if navi == 1 and mean > a.max_error:
            failures.append(f"{label} mean error {mean:.2f} > {a.max_error}")
    if path_length(host_pos, 1) < 50:
        failures.append("the guest captain barely moved on the host: input did not reach it")
    for name in ("host", "join"):
        if rcs.get(name) != 0:
            failures.append(f"{name} exit code {rcs.get(name)}")
    for failure in failures:
        print(f"run_online_pair: FAIL: {failure}")
    print("run_online_pair: " + ("PASS" if not failures else "FAIL"))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
