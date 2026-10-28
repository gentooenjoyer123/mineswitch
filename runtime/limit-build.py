#!/usr/bin/env python3
"""Run one project build at a time, within the owner's six-thread CPU budget."""

import fcntl
import os
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
LIMIT = 6


def bounded_arguments(arguments):
    result = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        match = re.fullmatch(r"-j([0-9]+)", value)
        if match:
            value = f"-j{min(LIMIT, max(1, int(match.group(1))))}"
        elif value in ("-j", "--jobs", "--parallel"):
            if index + 1 < len(arguments) and arguments[index + 1].isdigit():
                result.extend((value, str(min(LIMIT, max(1, int(arguments[index + 1]))))))
                index += 2
                continue
            value = f"-j{LIMIT}" if value == "-j" else f"{value}={LIMIT}"
        elif re.fullmatch(r"(?:--jobs|--parallel|JOBS)=[0-9]+", value):
            name, count = value.split("=", 1)
            value = f"{name}={min(LIMIT, max(1, int(count)))}"
        result.append(value)
        index += 1
    return result


def main():
    arguments = sys.argv[1:]
    if arguments and arguments[0] == "--":
        arguments = arguments[1:]
    if not arguments:
        raise SystemExit("Usage: python3 runtime/limit-build.py -- COMMAND [ARGS...]")
    arguments = bounded_arguments(arguments)
    environment = os.environ.copy()
    environment.update({
        "MINESWITCH_JOBS": "6", "MINESWITCH_BUILD_JOBS": "6",
        "CMAKE_BUILD_PARALLEL_LEVEL": "6",
    })
    java_options = re.sub(r"(?:^|\s)-XX:ActiveProcessorCount=\S+", "",
                          environment.get("_JAVA_OPTIONS", "")).strip()
    environment["_JAVA_OPTIONS"] = (java_options + " -XX:ActiveProcessorCount=6").strip()
    allowed = sorted(os.sched_getaffinity(0))
    selected = set(allowed[-min(LIMIT, len(allowed)):])
    os.sched_setaffinity(0, selected)
    os.setpriority(os.PRIO_PROCESS, 0, max(15, os.getpriority(os.PRIO_PROCESS, 0)))

    # Nested build scripts inherit limits and must not wait on their parent.
    nested = environment.get("MINESWITCH_BUILD_LIMITED") == "1"
    environment["MINESWITCH_BUILD_LIMITED"] = "1"
    lock_file = ROOT / ".build-resource.lock"
    with lock_file.open("a") as lock:
        if not nested:
            fcntl.flock(lock, fcntl.LOCK_EX)
            print(f"Build budget: {LIMIT} jobs, CPU set {','.join(map(str, sorted(selected)))}, nice 15",
                  file=sys.stderr, flush=True)
        # The parent alone owns the lock. A persistent javac daemon must not
        # inherit its descriptor and hold up subsequent builds.
        process = subprocess.Popen(arguments, env=environment, close_fds=True)
        try:
            code = process.wait()
        except KeyboardInterrupt:
            process.send_signal(2)
            code = process.wait()
        raise SystemExit(code)


if __name__ == "__main__":
    main()
