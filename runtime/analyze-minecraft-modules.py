#!/usr/bin/env python3
"""Derive JDK module roots from the verified Minecraft 1.21.1 Java artifacts."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / ".minecraft/1.21.1"
REPORT = ROOT / "runtime/minecraft-1.21.1-modules.json"
PROFILE = ROOT / "runtime/modules-minecraft-1.21.1.txt"
SERVICE_MODULES = {
    "jdk.crypto.ec": "Elliptic-curve TLS provider for the game's HTTPS authentication and downloads; service loading is not a static bytecode edge.",
    "jdk.naming.dns": "Minecraft's SRV resolver selects com.sun.jndi.dns.DnsContextFactory by class name.",
    "jdk.management": "Extended platform MXBeans and device validation use the real Horizon OS/process resource adapter.",
    "jdk.zipfs": "VanillaPackResourcesBuilder invokes FileSystems.newFileSystem for jar: URLs; ZipFS is a service provider.",
}
MOD_API_MODULES = {
    "java.net.http": "Debugify 1.21.1+1.0 BugFixDescriptionCache uses the genuine Java21 HttpClient API; observed in logs/MineSwitch-Launcher-20261006T102342Z.log.",
}


def digest(path: Path, algorithm: str) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, algorithm).hexdigest()


def main() -> None:
    boot = Path(os.environ.get("MINESWITCH_BOOT_JDK", "/opt/openjdk-bin-21"))
    version = json.loads((GAME / "version.json").read_text())
    libraries = json.loads((GAME / "library-inputs.json").read_text())
    paths = [Path(line) for line in (GAME / "classpath.txt").read_text().splitlines() if line]
    client = GAME / "client.jar"
    expected = {client: version["downloads"]["client"]}
    expected.update({GAME / "libraries" / lib["path"]: lib for lib in libraries})
    inputs = []
    for path in paths:
        metadata = expected[path]
        actual = digest(path, "sha1")
        if actual != metadata["sha1"] or path.stat().st_size != metadata["size"]:
            raise SystemExit(f"Official artifact verification failed: {path}")
        inputs.append({"path": str(path.relative_to(ROOT)), "sha1": actual, "bytes": path.stat().st_size})
    command = [str(boot / "bin/jdeps"), "-q", "--multi-release", "21",
               "--ignore-missing-deps", "--recursive", "--print-module-deps",
               "--class-path", os.pathsep.join(str(path) for path in paths if path != client), str(client)]
    analysis = subprocess.run(command, text=True, capture_output=True, check=True)
    (ROOT / "logs/minecraft-1.21.1-jdeps-modules.log").write_text(analysis.stdout + analysis.stderr)
    roots = sorted(set(analysis.stdout.strip().splitlines()[-1].split(",")))
    with zipfile.ZipFile(client) as archive:
        srv_classes = [name for name in archive.namelist() if name.endswith(".class")
                       and b"com.sun.jndi.dns.DnsContextFactory" in archive.read(name)]
        jar_classes = [name for name in archive.namelist() if name.endswith(".class")
                       and b"java/nio/file/FileSystems" in archive.read(name)
                       and b"newFileSystem" in archive.read(name)]
    if not srv_classes or not jar_classes:
        raise SystemExit("Expected Minecraft reflective DNS/ZipFS callsite evidence is absent")
    selected = sorted(set(roots) | set(SERVICE_MODULES) | set(MOD_API_MODULES))
    report = {
        "schema": 1, "minecraft_version": version["id"],
        "jdk_analysis_version": subprocess.check_output([str(boot / "bin/jdeps"), "--version"], text=True).strip(),
        "jdeps_options": command[1:7],
        "static_module_roots": roots,
        "service_roots": SERVICE_MODULES,
        "mod_api_roots": MOD_API_MODULES,
        "service_callsite_classes": {"jdk.naming.dns": srv_classes, "jdk.zipfs": jar_classes},
        "selected_roots": selected,
        "minecraft_boot_roots": [name for name in selected if name != "jdk.jfr"],
        "conditional_runtime_modules": {
            "jdk.jfr": "The image retains genuine JFR API classes for capability tests. Minecraft bnl (JvmProfiler) selects NoOp only when jdk.jfr is absent from the boot layer; its active profiler reads EventType despite a JFR-disabled VM. Exclude this module from Minecraft's resolved boot roots while native JFR is disabled, or apply an explicit application-side availability gate."
        },
        "inputs": inputs,
        "limitations": [
            "Static jdeps analysis does not discover arbitrary reflection or application-selected service providers.",
            "--ignore-missing-deps allows optional third-party integrations; it is not proof those integrations work.",
            "The classpath inventory includes OS-specific optional Java jars; native classifiers are not target binaries.",
            "The image builder derives transitive closure from the target module descriptors and records it separately.",
        ],
    }
    REPORT.write_text(json.dumps(report, indent=2) + "\n")
    PROFILE.write_text("# Derived from Minecraft 1.21.1 artifacts; see minecraft-1.21.1-modules.json.\n" +
                       "# Includes observed mod API requirements documented by that report.\n" +
                       "\n".join(selected) + "\n")
    (ROOT / "runtime/modules-minecraft-boot-1.21.1.txt").write_text(
        "# Runtime roots: JFR is unavailable in this VM, so let Minecraft select its NoOp profiler.\n" +
        "\n".join(name for name in selected if name != "jdk.jfr") + "\n")
    print(f"Verified {len(inputs)} official Java artifacts; selected {len(selected)} JDK module roots")
    print(",".join(selected))


if __name__ == "__main__":
    main()
