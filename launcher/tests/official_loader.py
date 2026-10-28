#!/usr/bin/env python3
"""Optional real Forge/NeoForge preparation check on PC; never starts Minecraft.

Uses official HTTPS repositories and executes the selected official Java tools.
Run through runtime/limit-build.py after building sd_tool and staging launcher-tools.jar.
The original client and tool runtime are copied into a dedicated development root.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "launcher/build-official-sd"
TOOL = ROOT / "launcher/build-host/sd_tool"
MC = ROOT / ".minecraft/1.21.1"

def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()

def run(arguments, timeout=900):
    print("RUN", " ".join(map(str, arguments)), flush=True)
    result = subprocess.run(list(map(str, arguments)), text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    print(result.stdout, end="", flush=True)
    if result.returncode:
        raise RuntimeError(f"Command failed with status {result.returncode}")
    return result.stdout

def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        raise SystemExit("Run through runtime/limit-build.py to respect the six-CPU build/test budget")
    loader = sys.argv[1] if len(sys.argv) == 2 else ""
    if loader not in ("forge", "neoforge"):
        raise SystemExit("Usage: official_loader.py forge|neoforge")
    version = "52.1.16" if loader == "forge" else "21.1.256"
    sentinel = WORK / ".official-loader-fixture"
    if WORK.exists() and not sentinel.is_file():
        raise SystemExit("Existing path is not this isolated fixture")
    WORK.mkdir(parents=True, exist_ok=True)
    sentinel.write_text("local development fixture; no console access\n")
    if not (WORK / "runtime").exists():
        shutil.copytree(ROOT / "runtime/generated-native-minecraft/romfs/jdk", WORK / "runtime")
    source_tools = ROOT / "runtime/generated-native-minecraft/romfs/jdk/mineswitch/launcher-tools.jar"
    if not source_tools.is_file():
        raise SystemExit("Stage the current launcher-tools.jar first")
    shutil.copyfile(source_tools, WORK / "runtime/mineswitch/launcher-tools.jar")
    game = WORK / "minecraft"
    game.mkdir(exist_ok=True)
    client = game / "client.jar"
    if not client.exists():
        shutil.copyfile(MC / "client.jar", client)
    if os.stat(client).st_ino == os.stat(MC / "client.jar").st_ino:
        raise SystemExit("The original client must be copied, not hardlinked")
    original_hash = digest(MC / "client.jar")
    if digest(client) != original_hash:
        raise SystemExit("Fixture client differs from the verified original")
    for folder in ("libraries", "logging"):
        if not (game / folder).exists():
            shutil.copytree(MC / folder, game / folder)
    seed = WORK / "runtime/mineswitch/metadata/1.21.1.json"
    seed.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(MC / "version.json", seed)
    instances = WORK / "instances"
    instance = next((p.parent for p in instances.glob("*/instance.json")
                     if json.loads(p.read_text()).get("name") == f"Official {loader} preparation check"), None)
    if instance is None:
        archive = WORK / f"{loader}-check.mrpack"
        manifest = {"formatVersion": 1, "game": "minecraft", "versionId": "local-check",
                    "name": f"Official {loader} preparation check", "files": [],
                    "dependencies": {"minecraft": "1.21.1", loader: version}}
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
            output.writestr("modrinth.index.json", json.dumps(manifest))
        text = run([TOOL, WORK, "import", archive])
        match = re.search(r"id=([0-9a-f]+)", text)
        if not match:
            raise RuntimeError("Importer did not return an instance ID")
        instance = instances / match.group(1)
    run([TOOL, WORK, "prepare", instance.name])
    launch = json.loads((instance / "launch-plan.json").read_text())
    if launch["preparation_only"]:
        if launch["main_class"] != "mineswitch.launcher.ProcessorEntry" or len(launch["game_arguments"]) != 1:
            raise RuntimeError("Unexpected processor launch plan")
        # Host JVM options are intentionally separate from Horizon JNI options.
        run(["java", "-Xms128m", "-Xmx1024m", "-XX:ActiveProcessorCount=6", "-cp",
             WORK / "runtime/mineswitch/launcher-tools.jar", launch["main_class"], launch["game_arguments"][0]])
    if digest(client) != original_hash or digest(MC / "client.jar") != original_hash:
        raise RuntimeError("Original client integrity changed")
    # Asset verification happens after the Java processor process has exited.
    # These immutable objects are never supplied as processor inputs or outputs.
    if not (game / "assets").exists():
        shutil.copytree(MC / "assets", game / "assets", copy_function=os.link)
    run([TOOL, WORK, "prepare", instance.name])
    launch = json.loads((instance / "launch-plan.json").read_text())
    expected = "net.minecraftforge.bootstrap.ForgeBootstrap" if loader == "forge" else "cpw.mods.bootstraplauncher.BootstrapLauncher"
    if launch["preparation_only"] or launch["main_class"] != expected:
        raise RuntimeError("Official loader has not reached a normal game launch plan")
    result = {"loader": loader, "version": version, "main": expected, "client_sha256": original_hash,
              "instance": str(instance), "minecraft_executed": False, "device_access": False}
    (WORK / f"{loader}-validation.json").write_text(json.dumps(result, indent=2) + "\n")
    print("OFFICIAL_LOADER_PREPARATION_PASS", json.dumps(result), flush=True)

if __name__ == "__main__":
    main()
