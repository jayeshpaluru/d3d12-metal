#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Summarises a frame timeline written by D3D12METAL_TIMELINE=<file.json>: frame time, GPU busy time (union of all
# queues) and idle gaps, per-queue GPU time and commit-to-start latency, CPU time per call per thread, and what the
# CPU threads were doing during the longest GPU idle gaps.
# usage: python3 -I tools/timeline_summary.py <file.json> [gaps to show]
import collections
import json
import sys


def union(intervals):
    out = []
    for b, e in sorted(intervals):
        if out and b <= out[-1][1]:
            out[-1][1] = max(out[-1][1], e)
        else:
            out.append([b, e])
    return out


def main():
    events = json.load(open(sys.argv[1]))["traceEvents"]
    show = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    names = {e["tid"]: e["args"]["name"] for e in events if e.get("ph") == "M" and e.get("name") == "thread_name"}
    cpu = [e for e in events if e.get("pid") == 1 and e.get("ph") in ("X", "i")]
    gpu = [e for e in events if e.get("pid") == 2 and e.get("ph") == "X"]
    presents = sorted(e["ts"] for e in cpu if e["name"] == "present")
    if len(presents) < 2:
        print("fewer than two presents")
        return
    t0, t1 = presents[0], presents[-1]
    frames = len(presents) - 1
    span = t1 - t0
    print(f"{frames} frames, {span / frames / 1000:.2f} ms per frame ({frames * 1e6 / span:.1f} fps)")

    clip = lambda b, e: (max(b, t0), min(e, t1))
    busy = union([clip(e["ts"], e["ts"] + e["dur"]) for e in gpu if e["ts"] + e["dur"] > t0 and e["ts"] < t1])
    busy_us = sum(e - b for b, e in busy)
    print(f"GPU busy (any queue) {busy_us / frames / 1000:.2f} ms per frame, idle {(span - busy_us) / frames / 1000:.2f} ms")
    per_queue = collections.defaultdict(list)
    for e in gpu:
        per_queue[e["tid"]].append(e)
    for q, evs in sorted(per_queue.items()):
        u = union([clip(e["ts"], e["ts"] + e["dur"]) for e in evs if e["ts"] + e["dur"] > t0 and e["ts"] < t1])
        lat = sorted(e["args"]["commit_to_start_us"] for e in evs)
        print(f"  queue {q}: {len(evs) / frames:.1f} command buffers per frame, busy {sum(e - b for b, e in u) / frames / 1000:.2f} ms,"
              f" commit-to-start median {lat[len(lat) // 2]:.0f} us, p90 {lat[int(len(lat) * 0.9)]:.0f} us")

    print("CPU per thread (ms per frame): call x count")
    per_thread = collections.defaultdict(lambda: collections.defaultdict(lambda: [0.0, 0]))
    for e in cpu:
        if e["ph"] == "X" and t0 <= e["ts"] < t1:
            s = per_thread[e["tid"]][e["name"]]
            s[0] += e["dur"]
            s[1] += 1
    for tid, calls in sorted(per_thread.items(), key=lambda kv: -sum(v[0] for v in kv[1].values())):
        desc = ", ".join(f"{n} {v[0] / frames / 1000:.2f} x{v[1] / frames:.1f}"
                         for n, v in sorted(calls.items(), key=lambda kv: -kv[1][0]))
        print(f"  {names.get(tid, tid)}: {desc}")

    gaps = [(busy[i + 1][0] - busy[i][1], busy[i][1], busy[i + 1][0]) for i in range(len(busy) - 1)]
    gaps.sort(reverse=True)
    print(f"GPU idle gaps: {len(gaps) / frames:.1f} per frame; longest {show}:")
    for length, b, e in gaps[:show]:
        print(f"  {length / 1000:.2f} ms at {(b - t0) / 1000:.2f} ms:")
        for ev in sorted(cpu, key=lambda x: x["ts"]):
            end = ev["ts"] + ev.get("dur", 0)
            if end >= b - 200 and ev["ts"] <= e:
                what = f'{ev["name"]}' + (f' ({ev["dur"] / 1000:.2f} ms)' if ev["ph"] == "X" else "")
                print(f"     {(ev['ts'] - b) / 1000:+7.2f} ms  {names.get(ev['tid'], ev['tid'])}: {what} v={ev['args'].get('v')}")
        starts = [g for g in gpu if b - 1 <= g["ts"] - (g["args"]["commit_to_start_us"]) <= e + 1]
        for g in starts:
            print(f"     GPU queue {g['tid']} buffer seq {g['args']['seq']} committed {(g['ts'] - g['args']['commit_to_start_us'] - b) / 1000:+.2f} ms,"
                  f" starts {(g['ts'] - b) / 1000:+.2f} ms")


main()
