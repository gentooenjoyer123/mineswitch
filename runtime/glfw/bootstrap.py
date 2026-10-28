#!/usr/bin/env python3
"""Prepare the pinned devkitPro GLFW source with the Horizon patch."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request


root = Path(__file__).resolve().parents[2]
metadata = json.loads((root / "runtime/glfw/SOURCE.json").read_text())
commit = metadata["commit"]
upstream = root / ".upstream"
upstream.mkdir(exist_ok=True)
source = upstream / "glfw-switch"
archive = upstream / f"glfw-switch-{commit}.tar.gz"
patch = root / "runtime/glfw/glfw-horizon.patch"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def git(*arguments, check=True):
    return subprocess.run(["git", "-C", str(source), *arguments], check=check,
                          capture_output=True, text=True)


if not source.exists():
    if not archive.exists() or digest(archive) != metadata["archive_sha256"]:
        temporary = archive.with_suffix(".partial")
        with urllib.request.urlopen(f"https://codeload.github.com/devkitPro/glfw/tar.gz/{commit}",
                                    timeout=40) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        if digest(temporary) != metadata["archive_sha256"]:
            temporary.unlink(missing_ok=True)
            raise SystemExit("GLFW source hash mismatch")
        temporary.replace(archive)
    with tarfile.open(archive, "r:gz") as bundle:
        members = bundle.getmembers()
        if any(Path(item.name).parts[0] != f"glfw-{commit}" for item in members):
            raise SystemExit("Unexpected GLFW archive root")
        bundle.extractall(upstream, members=members, filter="data")
    (upstream / f"glfw-{commit}").rename(source)
    git("init", "-q")
    git("add", "-A")
    git("-c", "user.name=MineSwitch Development", "-c", "user.email=mineswitch@localhost",
        "commit", "-q", "-m", f"Import devkitPro GLFW {commit}")

if git("apply", "--reverse", "--check", str(patch), check=False).returncode == 0:
    print("GLFW Horizon patch already present")
else:
    if git("status", "--porcelain").stdout.strip():
        raise SystemExit("Existing GLFW changes differ; refusing to overwrite them")
    git("apply", "--check", str(patch))
    git("apply", str(patch))
    print("Applied GLFW Horizon patch")
