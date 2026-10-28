#!/usr/bin/env python3
"""Create a pinned, personal controller-check mrpack; no device access or mod execution."""
import json
from pathlib import Path
import sys
import urllib.parse
import urllib.request
import zipfile

BASE = "https://api.modrinth.com/v2"
def get(path):
    request = urllib.request.Request(BASE + path, headers={"User-Agent": "MineSwitch/0.2.0 launcher development"})
    with urllib.request.urlopen(request, timeout=25) as response:
        return json.load(response)

pending = [get("/version/mZMBB8jx")]
versions = {}
while pending:
    version = pending.pop()
    if version["project_id"] in versions:
        continue
    if "1.21.1" not in version["game_versions"] or "fabric" not in version["loaders"]:
        raise SystemExit("A required dependency is not published for Fabric 1.21.1")
    versions[version["project_id"]] = version
    for dependency in version["dependencies"]:
        if dependency["dependency_type"] != "required":
            continue
        if dependency.get("version_id"):
            pending.append(get("/version/" + dependency["version_id"]))
        elif dependency.get("project_id"):
            query = urllib.parse.urlencode({"loaders": '["fabric"]', "game_versions": '["1.21.1"]'})
            available = get("/project/" + dependency["project_id"] + "/version?" + query)
            releases = [item for item in available if item["version_type"] == "release"]
            if not releases:
                raise SystemExit("No matching stable dependency release")
            pending.append(releases[0])
        else:
            raise SystemExit("Required dependency cannot be resolved by its published IDs")

files = []
for version in versions.values():
    candidates = [f for f in version["files"] if f["filename"].endswith(".jar")]
    primary = next((f for f in candidates if f.get("primary")), candidates[0])
    files.append({"path": "mods/" + primary["filename"], "hashes": primary["hashes"],
                  "downloads": [primary["url"]], "fileSize": primary["size"],
                  "env": {"client": "required", "server": "unsupported"}})

manifest = {"formatVersion": 1, "game": "minecraft", "versionId": "2026-10-06",
            "name": "MineSwitch Controller Check", "summary": "Personal device-validation pack; not yet tested on Horizon.",
            "dependencies": {"minecraft": "1.21.1", "fabric-loader": "0.19.5"}, "files": files}
output = Path(sys.argv[1] if len(sys.argv) > 1 else "dist/launcher-tests/controller-check.mrpack")
output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr("modrinth.index.json", json.dumps(manifest, indent=2))
output.with_suffix(".versions.json").write_text(json.dumps({key: {"id": v["id"], "name": v["name"]} for key,v in versions.items()}, indent=2) + "\n")
print(output)
print(json.dumps({"files": len(files), "download_bytes": sum(f["fileSize"] for f in files)}))
