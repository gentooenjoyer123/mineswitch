#!/usr/bin/env python3
"""Stage the local-profile bootstrap and real Horizon LWJGL jars for an NRO."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
MC = ROOT / ".minecraft/1.21.1"
GENERATED = Path(os.environ.get("MINESWITCH_GENERATED_DIR", ROOT / "runtime/generated")).resolve()
RUNTIME = GENERATED / "romfs/jdk"
SD_RUNTIME = "/switch/mineswitch/runtime"
SD_MC = "/switch/mineswitch/minecraft"


def allowed(rules):
    if not rules:
        return True
    result = False
    for rule in rules:
        platform = rule.get("os", {})
        if platform.get("name", "horizon") != "horizon":
            continue
        if platform.get("arch", "aarch64") not in ("aarch64", "arm64"):
            continue
        if "version" in platform or rule.get("features"):
            raise ValueError(f"Unsupported classpath rule must be reviewed: {rule}")
        result = rule["action"] == "allow"
    return result


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        raise SystemExit(subprocess.run([sys.executable, str(ROOT / "runtime/limit-build.py"),
            "--", sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]]).returncode)
    profile = json.loads((ROOT / "minecraft/profile.json").read_text())
    standalone = os.environ.get("MINESWITCH_STANDALONE") == "1"
    if profile["version"] != "1.21.1" or profile["mode"] != "offline":
        raise SystemExit("Unexpected game version or unsupported profile mode")
    if not re.fullmatch(r"[A-Za-z0-9_]{1,16}", profile["username"]):
        raise SystemExit("Invalid local username")
    if not 256 <= profile["heap_mib"] <= 5900:
        raise SystemExit("Switch Java heap must be between 256 and 5900 MiB; respect the device's process memory budget")
    if not (RUNTIME / "lib/modules").is_file():
        raise SystemExit("Build the genuine Java runtime image first")
    version = json.loads((MC / "version.json").read_text())
    policy = json.loads((ROOT / "minecraft/module-policy.json").read_text())
    libraries = json.loads((MC / "library-inputs.json").read_text())
    generated = GENERATED / "minecraft" if "MINESWITCH_GENERATED_DIR" in os.environ else ROOT / "minecraft/generated"
    classes = generated / "bootstrap-classes"
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir(parents=True, exist_ok=True)
    subprocess.run(["javac", "--release", "21", "-d", str(classes),
                    str(ROOT / "minecraft/src/mineswitch/launcher/ClientEntry.java"),
                    str(ROOT / "minecraft/src/mineswitch/launcher/GameMetrics.java"),
                    str(ROOT / "runtime/launcher/java/mineswitch/launcher/ProgressSampler.java")], check=True)
    sampler = GENERATED / "romfs/tests/mineswitch/launcher/ProgressSampler.class"
    sampler.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(classes / "mineswitch/launcher/ProgressSampler.class", sampler)
    application = RUNTIME / "mineswitch"
    application.mkdir(exist_ok=True)
    compatibility = application / "mineswitch-compat.jar"
    subprocess.run([sys.executable, str(ROOT / "launcher/compat/build.py"),
                    "--output", str(compatibility)], check=True)
    metadata_dir = application / "metadata"
    metadata_dir.mkdir(exist_ok=True)
    shutil.copyfile(MC / "version.json", metadata_dir / "1.21.1.json")
    processor_source = ROOT / "launcher/java/mineswitch/launcher/ProcessorEntry.java"
    if processor_source.is_file():
        processor_classes = generated / "processor-classes"
        if processor_classes.exists():
            shutil.rmtree(processor_classes)
        processor_classes.mkdir(parents=True, exist_ok=True)
        subprocess.run(["javac", "--release", "21", "-d", str(processor_classes),
                        str(processor_source)], check=True)
        subprocess.run(["jar", "--create", "--file", str(application / "launcher-tools.jar"),
                        "--date=2020-01-01T00:00:00Z",
                        "-C", str(processor_classes), "."], check=True)
    bootstrap = application / "minecraft-bootstrap.jar"
    subprocess.run(["jar", "--create", "--file", str(bootstrap), "--date=2020-01-01T00:00:00Z",
                    "-C", str(classes), "."], check=True)
    classpath = [SD_RUNTIME + "/mineswitch/minecraft-bootstrap.jar"]
    patched = {}
    for name in ("lwjgl", "lwjgl-opengl", "lwjgl-openal"):
        source = ROOT / "runtime/lwjgl/build" / f"{name}-3.3.3-horizon.jar"
        if not source.is_file():
            raise SystemExit(f"Missing actual Horizon LWJGL patch jar: {source}")
        destination = application / "libraries" / source.name
        destination.parent.mkdir(exist_ok=True)
        shutil.copyfile(source, destination)
        classpath.append(SD_RUNTIME + "/mineswitch/libraries/" + source.name)
        patched[name] = digest(source)
    classpath.append(SD_MC + "/client.jar")
    for library in libraries:
        coordinate = library["name"].split(":")
        if coordinate[0] == "org.lwjgl" and coordinate[1] in patched:
            continue
        if allowed(library["rules"]):
            classpath.append(SD_MC + "/libraries/" + library["path"])

    values = {
        "mode": "offline", "username": profile["username"], "version": "1.21.1",
        "gameDir": SD_MC + "/game", "assetsDir": SD_MC + "/assets",
        "assetIndex": version["assetIndex"]["id"],
        "width": str(profile["width"]), "height": str(profile["height"]),
        "quickPlayServer": profile["quick_play_server"],
    }
    if any("\n" in value or "\r" in value or "\\" in value for value in values.values()):
        raise SystemExit("Profile values must be single lines without property escapes")
    (application / "offline.properties").write_text("".join(f"{key}={value}\n" for key, value in values.items()))
    options = [
        "-Xms256m", f"-Xmx{profile['heap_mib']}m",
        "-Djava.class.path=" + ";".join(classpath),
        "-Dmineswitch.profile=" + ("/switch/mineswitch/profile.properties" if standalone
                                  else SD_RUNTIME + "/mineswitch/offline.properties"),
        "-Djava.awt.headless=true", "-Djava.net.preferIPv4Stack=true",
        "-Dorg.lwjgl.system.allocator=system", "-Dio.netty.transport.noNative=true",
        *policy["vm_options"],
    ]
    if os.environ.get("MINESWITCH_ENABLE_METRICS") == "1":
        options.append("-Dmineswitch.metrics=true")
    if not standalone:
        options.append("-Dmineswitch.progress=true")
    registry = json.loads((GENERATED / "native_registry.json").read_text())
    vm_variant = registry.get("jvm_variant", "zero")
    if vm_variant == "server":
        # CodeMemory is physically backed at reservation time. Bound this
        # initial client cache and use the single-heap mode tested by the probes.
        options.extend(["-XX:ReservedCodeCacheSize=96m", "-XX:InitialCodeCacheSize=8m",
                        "-XX:-SegmentedCodeCache", "-XX:CICompilerCount=2"])
    logging = version.get("logging", {}).get("client", {}).get("file")
    if logging:
        options.append("-Dlog4j.configurationFile=" + SD_MC + "/logging/" + logging["id"])
    launch = GENERATED / "romfs/launch"
    launch.mkdir(exist_ok=True)
    (launch / "minecraft.options").write_text("\n".join(options) + "\n")

    manifest = []
    for path in sorted(RUNTIME.rglob("*")):
        if path.is_file() and path.name != "files.manifest":
            relative = path.relative_to(RUNTIME).as_posix()
            manifest.append(f"{digest(path)} {path.stat().st_size} {relative}\n")
    (RUNTIME / "files.manifest").write_text("".join(manifest))
    (generated / "launch-summary.json").write_text(json.dumps({
        "mode": "offline", "main": "mineswitch.launcher.ClientEntry",
        "original_main": version["mainClass"], "classpath_entries": len(classpath),
        "heap_mib": profile["heap_mib"], "horizon_lwjgl_sha256": patched,
        "jvm_variant": vm_variant,
        "standalone_profile": standalone,
        "controller_adaptation": False,
        "fabric_compatibility_sha256": digest(compatibility),
        "vm_options": options,
    }, indent=2) + "\n")
    print(f"Prepared actual Minecraft launch: {len(classpath)} classpath entries, {len(manifest)} runtime files")


if __name__ == "__main__":
    main()
