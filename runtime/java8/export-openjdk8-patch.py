#!/usr/bin/env python3
"""Export the OpenJDK 8u Horizon changes (hotspot/ and jdk/, incl. new files).

The pinned baseline is the single import commit of .upstream/openjdk8
(.upstream/OPENJDK8_SOURCE.json). `--check` verifies that the exported patch
applies cleanly to that baseline in a temporary worktree-free index check.
"""

import argparse
from pathlib import Path
import subprocess


project = Path(__file__).resolve().parents[2]
source = project / ".upstream/openjdk8"
destination = project / "runtime/patches/openjdk8-horizon.patch"
PATHS = ["hotspot", "jdk"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="only verify the existing patch applies to the baseline")
    args = parser.parse_args()
    if not args.check:
        destination.parent.mkdir(exist_ok=True)
        subprocess.run(["git", "-C", str(source), "add", "-N", *PATHS], check=True)
        temporary = destination.with_suffix(".partial")
        with temporary.open("wb") as output:
            subprocess.run(["git", "-C", str(source), "diff", "--binary", "--", *PATHS],
                           stdout=output, check=True)
        if temporary.stat().st_size == 0:
            raise SystemExit("Refusing to replace the Horizon patch with an empty diff.")
        temporary.replace(destination)
        print(f"Exported {destination.stat().st_size} bytes to {destination}")
    # Apply against the pinned import commit using a scratch index (no checkout).
    base = subprocess.run(["git", "-C", str(source), "rev-list", "--max-parents=0", "HEAD"],
                          check=True, capture_output=True, text=True).stdout.split()[0]
    index = source / ".git/mineswitch-patch-check.index"
    env = {"GIT_INDEX_FILE": str(index), "PATH": "/usr/bin:/bin"}
    try:
        subprocess.run(["git", "-C", str(source), "read-tree", base], check=True, env=env)
        subprocess.run(["git", "-C", str(source), "apply", "--cached", "--check", str(destination)],
                       check=True, env=env)
    finally:
        index.unlink(missing_ok=True)
    print(f"Patch applies cleanly to baseline {base}")


if __name__ == "__main__":
    main()
