#!/usr/bin/env python3
"""Fetch the pinned OpenJDK 8u source (jdk8u492-ga) and apply the Horizon patch."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request


TAG = "jdk8u492-ga"
COMMIT = "9cecb1477fff90f2d367a4df0a94cb44510d1ba9"
ARCHIVE_SHA256 = "8e44d6a9c04f590f3f0c6a6b95a74733ba8c347f4c50993dea7d68e06861ff1e"
URL = f"https://codeload.github.com/openjdk/jdk8u/tar.gz/{COMMIT}"


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def git(directory, *args, check=True):
    return subprocess.run(["git", "-C", str(directory), *args], check=check,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main():
    project = Path(__file__).resolve().parents[2]
    upstream = project / ".upstream"
    source = upstream / "openjdk8"
    patch = project / "runtime/patches/openjdk8-horizon.patch"
    archive = upstream / f"jdk8u-{COMMIT}.tar.gz"
    upstream.mkdir(exist_ok=True)
    if not patch.is_file():
        raise SystemExit(f"Missing Horizon patch: {patch}")

    if not source.exists():
        if not archive.exists() or digest(archive) != ARCHIVE_SHA256:
            partial = archive.with_suffix(".partial")
            print("Downloading pinned OpenJDK 8u source...", flush=True)
            with urllib.request.urlopen(URL, timeout=60) as response, partial.open("wb") as output:
                shutil.copyfileobj(response, output)
            if digest(partial) != ARCHIVE_SHA256:
                partial.unlink(missing_ok=True)
                raise SystemExit("OpenJDK 8 source SHA256 mismatch.")
            partial.replace(archive)
        with tarfile.open(archive, "r:gz") as bundle:
            expected_root = f"jdk8u-{COMMIT}"
            members = bundle.getmembers()
            if any(Path(member.name).parts[0] != expected_root for member in members):
                raise SystemExit("Unexpected source archive root.")
            bundle.extractall(upstream, members=members, filter="data")
        (upstream / expected_root).rename(source)
        git(source, "init", "-q")
        git(source, "add", "-A")
        git(source, "-c", "user.name=MineSwitch Development", "-c",
            "user.email=mineswitch@localhost", "commit", "-q", "-m",
            f"Import pinned OpenJDK 8u {COMMIT} ({TAG})")

    if not (source / ".git").is_dir():
        raise SystemExit("Existing source has no baseline repository; refusing to overwrite it.")
    reverse = git(source, "apply", "--reverse", "--check", str(patch), check=False)
    if reverse.returncode == 0:
        print("Horizon patch is already present; preserving local work.")
    else:
        status = git(source, "status", "--porcelain").stdout
        if status.strip():
            raise SystemExit("Existing OpenJDK 8 source has local edits; refusing to overwrite them.")
        git(source, "apply", "--check", str(patch))
        git(source, "apply", str(patch))
        print("Applied Horizon source patch.")

    (upstream / "OPENJDK8_SOURCE.json").write_text(json.dumps({
        "repo": "https://github.com/openjdk/jdk8u", "tag": TAG, "commit": COMMIT,
        "archive_sha256": ARCHIVE_SHA256,
    }, indent=2) + "\n")
    print(f"OpenJDK 8 source ready: {source}")


if __name__ == "__main__":
    main()
