#!/usr/bin/env python3
"""Prepare a verified local ROMFS bundle of the official Minecraft inputs."""

import hashlib
import json
import os
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / ".minecraft/1.21.1"
PAYLOAD = ROOT / "minecraft/generated/romfs/minecraft"


def link_or_copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if os.path.samefile(source, destination):
            return
        destination.unlink()
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def main():
    version = json.loads((SOURCE / "version.json").read_text())
    libraries = json.loads((SOURCE / "library-inputs.json").read_text())
    index_id = version["assetIndex"]["id"]
    objects = json.loads((SOURCE / "assets/indexes" / f"{index_id}.json").read_text())["objects"]
    expected = {
        "client.jar": version["downloads"]["client"],
        f"assets/indexes/{index_id}.json": version["assetIndex"],
    }
    for library in libraries:
        expected["libraries/" + library["path"]] = library
    for asset in objects.values():
        digest = asset["hash"]
        expected[f"assets/objects/{digest[:2]}/{digest}"] = {
            "sha1": digest, "size": asset["size"]
        }
    logging = version.get("logging", {}).get("client", {}).get("file")
    if logging:
        expected["logging/" + logging["id"]] = logging

    PAYLOAD.mkdir(parents=True, exist_ok=True)
    lines = []
    size = 0
    for number, (relative, metadata) in enumerate(sorted(expected.items()), 1):
        source = SOURCE / relative
        if not source.is_file() or source.stat().st_size != metadata["size"]:
            raise SystemExit(f"Missing or wrong-sized game input: {relative}")
        sha1 = hashlib.sha1()
        sha256 = hashlib.sha256()
        with source.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                sha1.update(chunk)
                sha256.update(chunk)
        if sha1.hexdigest() != metadata["sha1"]:
            raise SystemExit(f"Official SHA1 mismatch: {relative}")
        destination = PAYLOAD / relative
        link_or_copy(source, destination)
        lines.append(f"{sha256.hexdigest()} {metadata['size']} {relative}\n")
        size += metadata["size"]
        if number % 500 == 0:
            print(f"Payload inputs verified: {number}/{len(expected)}", flush=True)

    profile = {
        "version": "1.21.1", "main_class": version["mainClass"], "asset_index": index_id,
        "classpath": ["client.jar"] + ["libraries/" + item["path"] for item in libraries],
        "logging": "logging/" + logging["id"] if logging else None,
        "java_major": version["javaVersion"]["majorVersion"],
        "authentication": "not configured by this installer",
    }
    profile_path = PAYLOAD / "inputs.json"
    profile_path.write_text(json.dumps(profile, indent=2) + "\n")
    contents = profile_path.read_bytes()
    lines.append(f"{hashlib.sha256(contents).hexdigest()} {len(contents)} inputs.json\n")
    size += len(contents)
    # A stale file must not be silently bundled after a changed artifact list.
    allowed = set(expected) | {"inputs.json", "files.manifest"}
    for path in PAYLOAD.rglob("*"):
        if path.is_file() and path.relative_to(PAYLOAD).as_posix() not in allowed:
            raise SystemExit(f"Unexpected stale payload file: {path}")
    (PAYLOAD / "files.manifest").write_text("".join(sorted(lines)))
    (ROOT / "minecraft/generated/payload-summary.json").write_text(json.dumps({
        "version": "1.21.1", "files": len(lines), "bytes": size,
        "target": "/switch/mineswitch/minecraft",
    }, indent=2) + "\n")
    print(f"Minecraft bundle prepared: {len(lines)} files, {size} bytes", flush=True)


if __name__ == "__main__":
    main()
