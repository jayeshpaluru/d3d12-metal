#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Fence latency in a D3D12METAL_TIMELINE trace: for each value a queue Signal set, when the GPU finished the command
# buffer carrying it, and when a thread first saw the value through GetCompletedValue (the front-end marks a poll that
# returns a new value). Prints the latency distribution and, per frame, the polls that unblocked the submitting thread.
# usage: python3 -I tools/timeline_fences.py <file.json>
import bisect
import json
import sys

events = json.load(open(sys.argv[1]))["traceEvents"]
names = {e["tid"]: e["args"]["name"] for e in events if e.get("ph") == "M" and e.get("name") == "thread_name"}
cpu = sorted((e for e in events if e.get("pid") == 1 and e.get("ph") in ("X", "i")), key=lambda e: e["ts"])
# GPU events are per queue (tid = queue id), and so are the commit sequence numbers: resolve the end by (queue, seq).
gpu_end = {(e["tid"], e["args"]["seq"]): e["ts"] + e["dur"] for e in events if e.get("pid") == 2 and e.get("ph") == "X"}

# Signals are followed on their thread by the commit of the buffer that carries them; with deferred commits a run of
# Signals ends in one commit, so every Signal since the last commit instant on the thread maps to it. A commit instant
# carries only its seq: its queue is taken from the thread's latest ExecuteCommandLists or Present (whose arg is the
# queue id), else queue 0.
signal_end = {}
pending = {}
queue_of = {}
for e in cpu:
    if e["name"] in ("ExecuteCommandLists", "Present"):
        queue_of[e["tid"]] = e["args"]["v"]
    elif e["name"] == "Signal":
        pending.setdefault(e["tid"], []).append(e["args"]["v"])
    elif e["name"] == "commit" and e["tid"] in pending:
        end = gpu_end.get((queue_of.get(e["tid"], 0), e["args"]["v"]))
        if end is not None:
            for value in pending[e["tid"]]:
                signal_end[value] = end
        del pending[e["tid"]]

lat = []
seen = []
for e in cpu:
    if e["name"] == "GetCompletedValue (new value)":
        v = e["args"]["v"]
        if v in signal_end:
            lat.append((e["ts"] - signal_end[v]) / 1000)
        seen.append((e["ts"], v, names.get(e["tid"], e["tid"])))
lat.sort()
if lat:
    print(f"GetCompletedValue saw a new value {len(lat)} times; GPU end -> seen: median {lat[len(lat) // 2]:.2f} ms,"
          f" p90 {lat[int(len(lat) * 0.9)]:.2f} ms, max {lat[-1]:.2f} ms")

# The submitting thread's resumptions after a quiet period, and the last poll result before each.
sub = [e for e in cpu if names.get(e["tid"]) == "Submit Thread" and e["ph"] == "X"]
seen_ts = [s[0] for s in seen]
for a, b in zip(sub, sub[1:]):
    quiet = b["ts"] - (a["ts"] + a["dur"])
    if quiet < 3000:
        continue
    i = bisect.bisect(seen_ts, b["ts"]) - 1
    if i >= 0:
        ts, v, who = seen[i]
        end = signal_end.get(v)
        gpu = f", GPU finished it {(ts - end) / 1000:.2f} ms before the poll" if end else ""
        print(f"quiet {quiet / 1000:5.1f} ms; resumes {(b['ts'] - ts) / 1000:5.2f} ms after {who} saw {v}{gpu}")
