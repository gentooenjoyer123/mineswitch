#!/usr/bin/env python3
"""Exercise the real SDL frontend with an ordinary local pack, without a network."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile
import io
import gzip
import struct


def nbt_string(value):
    data = value.encode()
    return struct.pack(">H", len(data)) + data


def nbt_level(name, game_type):
    """Minimal gzip level.dat: root{Data{LevelName, GameType, LastPlayed, Version{Name}}}."""
    data = b"\x08" + nbt_string("LevelName") + nbt_string(name)
    data += b"\x03" + nbt_string("GameType") + struct.pack(">i", game_type)
    data += b"\x04" + nbt_string("LastPlayed") + struct.pack(">q", 1760000000000)
    data += b"\x0a" + nbt_string("Version") + b"\x08" + nbt_string("Name") + nbt_string("1.21.1") + b"\x00"
    root = b"\x0a" + nbt_string("") + b"\x0a" + nbt_string("Data") + data + b"\x00" + b"\x00"
    return gzip.compress(root)

binary = Path(sys.argv[1]).resolve()
artifacts = Path(__file__).resolve().parents[1] / "build-host"
artifacts.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="mineswitch-ui-") as temporary:
    root = Path(temporary)
    archive = root / "test.mrpack"
    manifest = {
        "formatVersion": 1, "game": "minecraft", "versionId": "1.0",
        "name": "Local test pack", "dependencies": {"minecraft": "1.21.1", "fabric-loader":"0.19.3"}, "files": [],
    }
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as pack:
        pack.writestr("modrinth.index.json", json.dumps(manifest))
        pack.writestr("overrides/config/example.txt", "common settings\n")
        pack.writestr("client-overrides/config/example.txt", "client settings\n")
        mod=io.BytesIO()
        with zipfile.ZipFile(mod,"w",compression=zipfile.ZIP_DEFLATED) as jar:
            jar.writestr("fabric.mod.json",json.dumps({"schemaVersion":1,"id":"fixture","name":"Fixture Mod","version":"1.0"}))
        pack.writestr("client-overrides/mods/fixture.jar",mod.getvalue())
        pack.writestr("overrides/saves/FixtureWorld/level.dat", nbt_level("Fixture World", 1))
        resource=io.BytesIO()
        with zipfile.ZipFile(resource,"w") as rp:
            rp.writestr("pack.mcmeta",json.dumps({"pack":{"pack_format":34,"description":"§aFixture pack"}}))
        pack.writestr("overrides/resourcepacks/fixture-pack.zip",resource.getvalue())
        pack.writestr("overrides/logs/latest.log","[12:00:00] [main/INFO]: Loading\n[12:00:01] [main/WARN]: Fixture warning\n")
    # SDL documents this debug-only hint for leak checking: without it the
    # desktop D-Bus library keeps process-global caches after SDL unloads it.
    # This test exits immediately after SDL_Quit and starts no other D-Bus user.
    # A dead proxy keeps the test offline and deterministic: catalog and
    # version requests fail fast and the UI must stay usable without them.
    environment = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                       https_proxy="http://127.0.0.1:9", HTTPS_PROXY="http://127.0.0.1:9",
                       SDL_SHUTDOWN_DBUS_ON_QUIT="1")
    subprocess.run([str(binary), "--root", str(root / "sd"), "--test-archive", str(archive),
                    "--screenshot", str(artifacts / "ui-smoke.bmp")], env=environment, check=True, timeout=30)
