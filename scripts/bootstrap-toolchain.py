#!/usr/bin/env python3
"""Extract the pinned official devkitPro toolchain into this project (no root)."""

import hashlib
import json
from pathlib import Path
import platform
import tarfile
import urllib.request


REPOSITORY = "devkitpro/devkita64"
LAYER_SHA256 = "da5be4661c9ac4e389f9e5f939e58bff7d32bc60569fa7c14e849d5246881691"
LAYER_SIZE = 382982909
PREFIXES = (
    "opt/devkitpro/devkitA64/",
    "opt/devkitpro/libnx/",
    "opt/devkitpro/tools/",
    "opt/devkitpro/portlibs/switch/",
)


def sha256_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise SystemExit("This pinned toolchain is for x86_64 Linux hosts.")
    root = Path(__file__).resolve().parent.parent / ".toolchain"
    root.mkdir(exist_ok=True)
    archive = root / f"{LAYER_SHA256}.tar.gz"

    if not archive.exists() or sha256_file(archive) != LAYER_SHA256:
        token_url = (
            "https://auth.docker.io/token?service=registry.docker.io"
            f"&scope=repository:{REPOSITORY}:pull"
        )
        with urllib.request.urlopen(token_url, timeout=30) as response:
            token = json.load(response)["token"]
        blob_url = (
            f"https://registry-1.docker.io/v2/{REPOSITORY}/blobs/"
            f"sha256:{LAYER_SHA256}"
        )
        request = urllib.request.Request(
            blob_url, headers={"Authorization": f"Bearer {token}"}
        )
        partial = archive.with_suffix(".partial")
        received = 0
        digest = hashlib.sha256()
        print("Downloading official devkitPro toolchain (365 MiB)...", flush=True)
        with urllib.request.urlopen(request, timeout=60) as response:
            with partial.open("wb") as output:
                while chunk := response.read(1024 * 1024):
                    output.write(chunk)
                    digest.update(chunk)
                    received += len(chunk)
        if received != LAYER_SIZE or digest.hexdigest() != LAYER_SHA256:
            partial.unlink(missing_ok=True)
            raise SystemExit("Toolchain layer size or SHA256 mismatch.")
        partial.replace(archive)

    print("SHA256 verified. Extracting compiler, libnx, build tools, and Switch portlibs...", flush=True)
    count = 0
    with tarfile.open(archive, "r|gz") as layer:
        for member in layer:
            if member.name.startswith(PREFIXES):
                layer.extract(member, root, filter="data")
                count += 1
    print(f"Extracted {count} entries into {root / 'opt/devkitpro'}", flush=True)


if __name__ == "__main__":
    main()
