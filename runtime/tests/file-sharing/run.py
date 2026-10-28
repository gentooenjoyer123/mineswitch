#!/usr/bin/env python3
"""Build the actual file bridge with a host SDK model; never access a device."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent

def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        raise SystemExit("Use python3 runtime/limit-build.py -- python3 runtime/tests/file-sharing/run.py")
    build = HERE / "build"
    build.mkdir(exist_ok=True)
    flags = ["gcc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(HERE / "include"), "-I" + str(ROOT / "runtime/io")]
    aliases = ["open", "close", "read", "write", "lseek", "fsync", "ftruncate", "fstat", "stat", "lstat"]
    objects = []
    for name in ("horizon_file_io", "horizon_file_share"):
        target = build / (name + ".host.o")
        # stat/lstat are function-like aliases to retain `struct stat` spelling.
        definitions = ["-D" + name + "=nx_test_" + name for name in aliases if name not in ("stat", "lstat")]
        definitions += ["-Dstat(path,info)=nx_test_stat(path,info)", "-Dlstat(path,info)=nx_test_lstat(path,info)"]
        subprocess.run(flags + definitions + ["-c", str(ROOT / "runtime/io" / (name + ".c")), "-o", str(target)], check=True)
        objects.append(str(target))
    binary = build / "file-sharing-host"
    subprocess.run(flags + [str(HERE / "sdk_shim.c"), str(HERE / "main.c")] + objects + ["-o", str(binary)], check=True)
    with tempfile.TemporaryDirectory(prefix="mineswitch-file-sharing-") as temporary:
        subprocess.run([str(binary)], cwd=temporary, check=True, timeout=30)

if __name__ == "__main__":
    main()
