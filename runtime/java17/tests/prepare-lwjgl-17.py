#!/usr/bin/env python3
"""Stage the LWJGL 3.3.3 Horizon probe for the Java 17 LWJGL NRO (no device access).

Compiles runtime/lwjgl/tests/LwjglProbe.java with --release 17 and stages it,
the Horizon LWJGL jars (built with --release 8 by runtime/lwjgl/build_java.py)
and the upstream GLFW/STB 3.3.3 jars into runtime/java17/generated so the
probe is self-contained under sdmc:/switch/mineswitch/runtime-17/.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BUILD = HERE / "build/lwjgl"
BOOT = Path(os.environ.get("MINESWITCH_BOOT_JDK17", "/opt/openjdk-bin-17"))
GENERATED = Path(os.environ.get("MINESWITCH_JDK17_GENERATED_DIR", ROOT / "runtime/java17/generated"))
SD_LIBS = "/switch/mineswitch/runtime-17/mineswitch/libraries"
LIBRARIES = [
    ROOT / "runtime/lwjgl/build/lwjgl-3.3.3-horizon.jar",
    ROOT / "runtime/lwjgl/build/lwjgl-opengl-3.3.3-horizon.jar",
    ROOT / ".minecraft/1.21.1/libraries/org/lwjgl/lwjgl-glfw/3.3.3/lwjgl-glfw-3.3.3.jar",
    ROOT / ".minecraft/1.21.1/libraries/org/lwjgl/lwjgl-stb/3.3.3/lwjgl-stb-3.3.3.jar",
]


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                                 "--", sys.executable, __file__, *sys.argv[1:]])
    for library in LIBRARIES:
        if not library.is_file():
            raise SystemExit(f"Missing LWJGL input: {library}")
    runtime = GENERATED / "romfs/jdk"
    if not (runtime / "lib/modules").is_file():
        raise SystemExit("Build the Java 17 LWJGL image first (see docs/JAVA17_PORT.md)")
    classes = BUILD / "classes"
    shutil.rmtree(classes, ignore_errors=True)
    classes.mkdir(parents=True)
    run(BOOT / "bin/javac", "--release", "17", "-proc:none", "-cp", os.pathsep.join(map(str, LIBRARIES)),
        "-d", classes, ROOT / "runtime/lwjgl/tests/LwjglProbe.java")
    probe_jar = BUILD / "lwjgl-probe.jar"
    run(BOOT / "bin/jar", "--create", "--file", probe_jar, "-C", classes, ".")
    target = runtime / "mineswitch/libraries"
    target.mkdir(parents=True, exist_ok=True)
    for library in [*LIBRARIES, probe_jar]:
        shutil.copyfile(library, target / library.name)
    launch = GENERATED / "romfs/launch"
    launch.mkdir(exist_ok=True)
    classpath = ";".join(f"{SD_LIBS}/{library.name}" for library in [probe_jar, *LIBRARIES])
    (launch / "lwjgl-probe.options").write_text("\n".join([
        "-Djava.class.path=" + classpath,
        "-Dorg.lwjgl.util.Debug=true",
        "-Djava.awt.headless=true",
        "-XX:ReservedCodeCacheSize=128m",
        "-XX:InitialCodeCacheSize=8m",
        "-XX:-SegmentedCodeCache",
        "-XX:CICompilerCount=2",
    ]) + "\n")
    manifest = []
    for path in sorted(runtime.rglob("*")):
        if path.is_file() and path.name != "files.manifest":
            with path.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            manifest.append(f"{digest} {path.stat().st_size} {path.relative_to(runtime).as_posix()}\n")
    (runtime / "files.manifest").write_text("".join(manifest))
    print(f"Staged Java 17 LWJGL probe; {len(manifest)} runtime files in manifest")


if __name__ == "__main__":
    main()
