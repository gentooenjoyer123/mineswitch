#!/usr/bin/env python3
"""Snapshot one explicitly selected standalone NRO into the owned installer payload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PAYLOAD = HERE / "generated/romfs/package"
MAX_BYTES = 128 * 1024 * 1024


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                                "--", sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nro", required=True, type=Path, help="final standalone Minecraft NRO")
    parser.add_argument("--expect-sha256", help="optional expected final-candidate SHA256")
    args = parser.parse_args()
    source = args.nro.resolve(strict=True)
    if not source.is_file() or source.suffix.lower() != ".nro":
        parser.error("--nro must name a regular NRO file")
    PAYLOAD.mkdir(parents=True, exist_ok=True)
    allowed = {"MineSwitch.nro", "files.manifest"}
    unexpected = [p for p in PAYLOAD.iterdir() if p.name not in allowed or not p.is_file() or p.is_symlink()]
    if unexpected:
        parser.error("unexpected payload files; refusing to bundle stale data: " + ", ".join(map(str, unexpected)))
    destination = PAYLOAD / "MineSwitch.nro"
    if source == destination.resolve():
        parser.error("select the final launcher artifact, not the installer staging copy")
    with source.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if not 0x80 <= before.st_size <= MAX_BYTES:
            parser.error("standalone NRO must be 128 bytes..128 MiB; the game-data installer is not a package input")
        header = stream.read(0x80)
        if header[0x10:0x14] != b"NRO0":
            parser.error("input has no NRO0 header")
        executable_size = struct.unpack_from("<I", header, 0x18)[0]
        if not 0x80 <= executable_size <= before.st_size - 0x38:
            parser.error("invalid NRO executable/asset bounds")
        stream.seek(executable_size)
        assets = stream.read(0x38)
        if assets[:4] != b"ASET":
            parser.error("final launcher must contain its embedded runtime ROMFS")
        romfs_offset, romfs_size = struct.unpack_from("<QQ", assets, 0x28)
        if not romfs_size or romfs_offset < 0x38 or executable_size + romfs_offset + romfs_size > before.st_size:
            parser.error("invalid or missing embedded ROMFS")
        stream.seek(0)
        with tempfile.TemporaryDirectory(prefix=".stage-", dir=HERE / "generated") as temp:
            staged = Path(temp) / "MineSwitch.nro"
            digest = hashlib.sha256()
            copied = 0
            with staged.open("wb") as output:
                while chunk := stream.read(1024 * 1024):
                    copied += len(chunk)
                    output.write(chunk)
                    digest.update(chunk)
                output.flush()
                os.fsync(output.fileno())
            after = os.fstat(stream.fileno())
            if copied != before.st_size or (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise SystemExit("source NRO changed while snapshotting; retry after the launcher build finishes")
            sha = digest.hexdigest()
            if args.expect_sha256 and sha != args.expect_sha256.lower():
                raise SystemExit("final-candidate SHA256 mismatch")
            staged.replace(destination)
            manifest = Path(temp) / "files.manifest"
            manifest.write_text(f"{sha} {copied} MineSwitch.nro\n")
            manifest.replace(PAYLOAD / "files.manifest")
    summary = {"source": str(source), "files": 1, "bytes": copied, "sha256": sha,
               "target": "/switch/mineswitch/MineSwitch.nro", "game_data_included": False}
    (HERE / "generated/package-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
