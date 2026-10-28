#!/usr/bin/env python3
"""Compile genuine Newlib variants over a bounded host arena, never a device."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--expect-unpatched", action="store_true")
    args = parser.parse_args()
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        raise SystemExit("Run through python3 runtime/limit-build.py -- python3 runtime/memory/newlib/tests/run.py ...")
    source = args.source_dir.resolve() / "_mallocr.c"
    if not source.is_file():
        raise SystemExit(f"Missing allocator source: {source}")
    build = HERE / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="host-", dir=build) as temporary:
        work = Path(temporary)
        flags = ["gcc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-fno-builtin",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread", "-Werror",
                 "-I" + str(HERE / "include")]
        # Host libc headers must be read before _LIBC, which intentionally
        # selects Newlib's reentrant separate-object allocator ABI below.
        preload = "\n".join(f"#include <{h}>" for h in (
            "stdio.h", "limits.h", "errno.h", "unistd.h", "stdint.h", "stddef.h", "assert.h"))
        variants = ("MALLOC", "REALLOC", "FREE", "MEMALIGN", "CALLOC", "MALLINFO", "MALLOPT")

        def compile_variant(variant):
            wrapper = work / (variant.lower() + ".c")
            wrapper.write_text(preload + f"\n#undef assert\n#define _LIBC 1\n#define DEFINE_{variant} 1\n#include " + json.dumps(str(source)) + "\n")
            target = wrapper.with_suffix(".o")
            subprocess.run(flags + ["-c", str(wrapper), "-o", str(target)], check=True)
            return target

        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            objects = list(pool.map(compile_variant, variants))
        wrapper = work / "free_snapshot.c"
        wrapper.write_text(preload + '\n#undef assert\n#include ' +
                           json.dumps(str(HERE.parent / "free_snapshot.c")) + '\n')
        snapshot = wrapper.with_suffix('.o')
        subprocess.run(flags + ['-c', str(wrapper), '-o', str(snapshot)], check=True)
        objects.append(snapshot)
        binary = work / "newlib-host"
        subprocess.run(flags + ["-Wall", "-Wextra", "-Werror", str(HERE / "main.c")] +
                       [str(p) for p in objects] + ["-o", str(binary)], check=True)
        cases = ("large", "invalid", "aligned-tail", "aligned-prefix") if args.expect_unpatched else ("large", "invalid", "top-pad", "small", "metaspace", "aligned-tail", "aligned-prefix", "free-snapshot", "snapshot-threads")
        checks = 0
        for case in cases:
            result = subprocess.run([str(binary), case], text=True, capture_output=True, timeout=60)
            output = result.stdout + result.stderr
            if args.expect_unpatched:
                expected = ("FAIL large aligned allocation above 2 GiB" if case == "large" else
                            "FAIL malloc lock balance" if case == "invalid" else
                            "FAIL exact aligned interval survives worst-case-padding failure")
                if result.returncode == 0 or expected not in output:
                    raise SystemExit(f"Unpatched negative control did not expose {case}:\n{output}")
                print(f"NEWLIB_HOST_NEGATIVE_CONTROL_PASS case={case}")
            else:
                if result.returncode:
                    raise SystemExit(f"Allocator host case {case} failed ({result.returncode}):\n{output}")
                print(output.strip())
                checks += int(re.search(r"checks=(\d+)", output).group(1))
        print("NEWLIB_HOST_UNPATCHED_REJECTED" if args.expect_unpatched else f"NEWLIB_HOST_PASS checks={checks}")


if __name__ == "__main__":
    main()
