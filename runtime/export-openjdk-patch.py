#!/usr/bin/env python3
"""Export current target changes, including newly added platform source files."""

from pathlib import Path
import subprocess


project = Path(__file__).resolve().parent.parent
source = project / ".upstream/openjdk21"
destination = project / "runtime/patches/openjdk21-horizon.patch"
destination.parent.mkdir(exist_ok=True)
subprocess.run(["git", "-C", str(source), "add", "-N", "make", "src"], check=True)
temporary = destination.with_suffix(".partial")
with temporary.open("wb") as output:
    subprocess.run(["git", "-C", str(source), "diff", "--binary", "--", "make", "src"],
                   stdout=output, check=True)
if temporary.stat().st_size == 0:
    raise SystemExit("Refusing to replace the Horizon patch with an empty diff.")
temporary.replace(destination)
print(f"Exported {destination.stat().st_size} bytes to {destination}")
