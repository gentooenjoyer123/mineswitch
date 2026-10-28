#!/usr/bin/env python3
"""Fetch Minecraft 1.21.1 inputs from Mojang, verifying published hashes.

This prepares local development inputs. It neither authenticates a player nor
launches the game, and the downloaded game files are not source deliverables.
"""

from concurrent.futures import ThreadPoolExecutor, as_completed
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import time
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
VERSION = "1.21.1"
VERSION_SHA1 = "22a1966494dfa4eeb5ee778c8e6ed5b774839582"
VERSION_URL = f"https://piston-meta.mojang.com/v1/packages/{VERSION_SHA1}/{VERSION}.json"


def sha1_file(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha1").hexdigest()


def verified_download(url, path, expected_sha1, expected_size=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and (expected_size is None or path.stat().st_size == expected_size):
        if sha1_file(path) == expected_sha1:
            return False
    temporary = path.with_name(path.name + ".partial")
    for attempt in range(4):
        try:
            digest = hashlib.sha1()
            size = 0
            request = urllib.request.Request(url, headers={"User-Agent": "MineSwitch-development/0.1"})
            with urllib.request.urlopen(request, timeout=45) as response, temporary.open("wb") as output:
                while chunk := response.read(1024 * 1024):
                    output.write(chunk)
                    digest.update(chunk)
                    size += len(chunk)
            if digest.hexdigest() != expected_sha1:
                raise ValueError(f"SHA1 mismatch for {path.name}")
            if expected_size is not None and size != expected_size:
                raise ValueError(f"Size mismatch for {path.name}: {size} != {expected_size}")
            temporary.replace(path)
            return True
        except (OSError, ValueError, urllib.error.URLError):
            temporary.unlink(missing_ok=True)
            if attempt == 3:
                raise
            time.sleep(min(2 ** attempt, 4))
    return False


def safe_artifact_path(relative):
    path = PurePosixPath(relative)
    if path.is_absolute() or ".." in path.parts or "\\" in relative:
        raise ValueError(f"Invalid artifact path: {relative!r}")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", action="store_true", help="also download asset objects")
    parser.add_argument("--workers", type=int, default=6)
    args = parser.parse_args()
    if not 1 <= args.workers <= 12:
        parser.error("--workers must be between 1 and 12")
    work = ROOT / ".minecraft" / VERSION
    manifest = work / "version.json"
    verified_download(VERSION_URL, manifest, VERSION_SHA1)
    version = json.loads(manifest.read_text())
    if version["id"] != VERSION or version["javaVersion"]["majorVersion"] != 21:
        raise SystemExit("Unexpected Minecraft version or Java requirement.")

    tasks = []
    client = version["downloads"]["client"]
    tasks.append((client["url"], work / "client.jar", client["sha1"], client["size"]))
    if "client_mappings" in version["downloads"]:
        mappings = version["downloads"]["client_mappings"]
        tasks.append((mappings["url"], work / "client-mappings.txt", mappings["sha1"], mappings["size"]))
    library_metadata = []
    classpath = []
    for library in version["libraries"]:
        artifact = library.get("downloads", {}).get("artifact")
        if not artifact:
            continue
        # Native classifiers are OS binaries, not Java dependencies for Horizon.
        # Keep metadata about them, but never load desktop binaries into the port.
        coordinate = library["name"]
        if len(coordinate.split(":")) > 3 and coordinate.split(":")[3].startswith("natives-"):
            continue
        relative = safe_artifact_path(artifact["path"])
        destination = work / "libraries" / relative
        tasks.append((artifact["url"], destination, artifact["sha1"], artifact["size"]))
        classpath.append(str(destination.resolve()))
        library_metadata.append({
            "name": coordinate, "path": str(relative), "sha1": artifact["sha1"],
            "size": artifact["size"], "rules": library.get("rules", []),
        })
    asset_index = version["assetIndex"]
    index_path = work / "assets/indexes" / f"{asset_index['id']}.json"
    tasks.append((asset_index["url"], index_path, asset_index["sha1"], asset_index["size"]))
    logging_file = version.get("logging", {}).get("client", {}).get("file")
    if logging_file:
        tasks.append((logging_file["url"], work / "logging" / logging_file["id"],
                      logging_file["sha1"], logging_file["size"]))

    print(f"Verifying/downloading client, mappings, {len(classpath)} library artifacts and metadata...", flush=True)
    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = [executor.submit(verified_download, *task) for task in tasks]
        downloaded = 0
        for future in as_completed(futures):
            downloaded += future.result()
    classpath.insert(0, str((work / "client.jar").resolve()))
    (work / "classpath.txt").write_text("\n".join(classpath) + "\n")
    (work / "library-inputs.json").write_text(json.dumps(library_metadata, indent=2) + "\n")
    print(f"Client/library inputs ready: {work}; downloaded {downloaded} files.", flush=True)

    if args.assets:
        objects = json.loads(index_path.read_text())["objects"]
        unique = {item["hash"]: item["size"] for item in objects.values()}
        total_size = sum(unique.values())
        print(f"Verifying/downloading {len(unique)} asset objects ({total_size} bytes)...", flush=True)
        completed = 0
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            futures = []
            for digest, size in unique.items():
                relative = f"{digest[:2]}/{digest}"
                futures.append(executor.submit(verified_download,
                    f"https://resources.download.minecraft.net/{relative}",
                    work / "assets/objects" / relative, digest, size))
            for future in as_completed(futures):
                future.result()
                completed += 1
                if completed % 100 == 0 or completed == len(unique):
                    print(f"Assets verified: {completed}/{len(unique)}", flush=True)
    print("Minecraft inputs verified. Authentication and Horizon native integration are separate steps.", flush=True)


if __name__ == "__main__":
    main()
