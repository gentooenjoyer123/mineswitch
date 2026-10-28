#!/usr/bin/env python3
"""Summarize an explicitly selected gameplay interval from a device log.

The caller must verify the scene. Frame samples measure completed swap
intervals, including waits, and contain no per-frame percentile information.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re


def values(line):
    return {key: float(value) if "." in value else int(value)
            for key, value in re.findall(r"(\w+)=(-?\d+(?:\.\d+)?)", line)}


def summarize(path, start, end):
    samples, memory, progress, cpu, gc = [], [], [], [], []
    pools = {}
    elapsed = None
    for line in path.read_text(errors="replace").splitlines():
        match = re.search(r"JVM_HEARTBEAT elapsed_s=(\d+)", line)
        if match:
            elapsed = int(match[1])
        if elapsed is None or not start <= elapsed <= end:
            continue
        row = {"elapsed_s": elapsed, **values(line)}
        if "MINESWITCH_FRAME_TIME " in line:
            samples.append(row)
        elif "MINESWITCH_NATIVE_MEMORY " in line:
            memory.append(row)
        elif "MINESWITCH_PROGRESS " in line:
            progress.append(row)
        elif "MINESWITCH_MAIN_CPU ns=" in line:
            cpu.append(row)
        elif "MINESWITCH_MEMORY_POOL " in line:
            name = re.search(r"name=(.*?) used=", line)[1]
            pools.setdefault(name, []).append(row)
        elif "[gc" in line and "Pause " in line:
            pause = re.search(r"([\d.]+)ms", line)
            if pause:
                gc.append({"elapsed_s": elapsed, "pause_ms": float(pause[1]),
                           "full": "Pause Full" in line})
    if not samples:
        raise ValueError("No frame samples in selected interval")

    def span(rows, key):
        found = [row[key] for row in rows if key in row]
        return {"min": min(found), "max": max(found), "last": found[-1]} if found else None

    observed = sum(row["observed_ms"] for row in samples)
    result = {
        "source": str(path), "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "requested_start_s": start, "requested_end_s": end,
        "sample_count": len(samples), "observed_s": observed / 1000,
        "weighted_fps": sum(row["frames"] for row in samples) * 1000 / observed,
        "window_fps": span(samples, "fps"),
        "interval_max_ms": max(row["max_ms"] for row in samples),
        "over50": sum(row["over50"] for row in samples),
        "over100": sum(row["over100"] for row in samples),
        "native_available_bytes": span(memory, "available"),
        "native_free_arena_bytes": span(memory, "free_arena"),
        "process_limit_bytes": span(memory, "limit"),
        "heap_used_bytes": span(progress, "heap_used"),
        "heap_committed_bytes": span(progress, "heap_committed"),
        "pools": {name: {key: span(rows, key) for key in ("used", "committed", "max")}
                  for name, rows in pools.items()},
        "gc": {"count": len(gc), "full_count": sum(row["full"] for row in gc),
               "total_pause_ms": sum(row["pause_ms"] for row in gc),
               "max_pause_ms": max((row["pause_ms"] for row in gc), default=0)},
        "notes": ["Scene and movement must be confirmed externally.",
                  "Samples are completed-swap intervals, not GPU render time.",
                  "Native available is aggregate, not the largest free block.",
                  "Non-frame rows are assigned to the preceding heartbeat; boundaries are approximate."],
        "frames": samples, "native_memory": memory, "java_progress": progress,
        "memory_pools": pools, "main_cpu": cpu, "gc_events": gc,
    }
    if len(cpu) > 1 and cpu[-1]["elapsed_s"] > cpu[0]["elapsed_s"]:
        result["main_thread_core_fraction"] = ((cpu[-1]["ns"] - cpu[0]["ns"]) / 1e9 /
                                               (cpu[-1]["elapsed_s"] - cpu[0]["elapsed_s"]))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--start", type=int, required=True)
    parser.add_argument("--end", type=int, required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = summarize(args.log, args.start, args.end)
    result["scene"] = args.scene
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items()
                      if key not in ("frames", "native_memory", "java_progress", "memory_pools", "main_cpu", "gc_events")}, indent=2))
