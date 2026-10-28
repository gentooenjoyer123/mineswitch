#!/usr/bin/env python3
"""Stage the LWJGL 3.3.3 Horizon probe for the Java 8 LWJGL NRO (no device access).

Run after `MINESWITCH_JDK8_GENERATED_DIR=runtime/java8/generated-lwjgl
runtime/java8/prepare-runtime-8.py`. Compiles runtime/lwjgl/tests/LwjglProbe.java
for Java 8 (-source/-target 8 against the pinned rt.jar) and stages it, the
Horizon LWJGL jars (class version 52, runtime/lwjgl/build_java.py) and the
upstream GLFW/STB 3.3.3 jars under sdmc:/switch/mineswitch/runtime-8/.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BUILD = HERE / "build/lwjgl"
BOOT = Path(os.environ.get("MINESWITCH_BOOT_JDK8", "/opt/openjdk-bin-8"))
GENERATED = Path(os.environ.get("MINESWITCH_JDK8_GENERATED_DIR", ROOT / "runtime/java8/generated-lwjgl"))
SD_LIBS = "/switch/mineswitch/runtime-8/mineswitch/libraries"
LIBRARIES = [
    ROOT / "runtime/lwjgl/build/lwjgl-3.3.3-horizon.jar",
    ROOT / "runtime/lwjgl/build/lwjgl-opengl-3.3.3-horizon.jar",
    ROOT / ".minecraft/1.21.1/libraries/org/lwjgl/lwjgl-glfw/3.3.3/lwjgl-glfw-3.3.3.jar",
    ROOT / ".minecraft/1.21.1/libraries/org/lwjgl/lwjgl-stb/3.3.3/lwjgl-stb-3.3.3.jar",
]


def run(*args):
    env = dict(os.environ)
    env.pop("_JAVA_OPTIONS", None)
    subprocess.run([str(arg) for arg in args], check=True, env=env)


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                                 "--", sys.executable, __file__, *sys.argv[1:]])
    for library in LIBRARIES:
        if not library.is_file():
            raise SystemExit(f"Missing LWJGL input: {library}")
    runtime = GENERATED / "romfs/jdk"
    if not (runtime / "lib/rt.jar").is_file():
        raise SystemExit("Prepare the Java 8 runtime ROMFS first (see docs/JAVA8_PORT.md)")
    classes = BUILD / "classes"
    shutil.rmtree(classes, ignore_errors=True)
    classes.mkdir(parents=True)
    run(BOOT / "bin/javac", "-source", "8", "-target", "8", "-proc:none", "-nowarn",
        "-bootclasspath", BOOT / "jre/lib/rt.jar", "-cp", os.pathsep.join(map(str, LIBRARIES)),
        "-d", classes, ROOT / "runtime/lwjgl/tests/LwjglProbe.java")
    probe_jar = BUILD / "lwjgl-probe.jar"
    with zipfile.ZipFile(probe_jar, "w", zipfile.ZIP_DEFLATED) as jar:
        for path in sorted(classes.rglob("*.class")):
            info = zipfile.ZipInfo(path.relative_to(classes).as_posix(), (1980, 1, 1, 0, 0, 0))
            jar.writestr(info, path.read_bytes(), zipfile.ZIP_DEFLATED)
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
        "-XX:CICompilerCount=2",
    ]) + "\n")
    manifest = []
    for path in sorted(runtime.rglob("*")):
        if path.is_file() and path.name != "files.manifest":
            with path.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            manifest.append(f"{digest} {path.stat().st_size} {path.relative_to(runtime).as_posix()}\n")
    (runtime / "files.manifest").write_text("".join(manifest))
    print(f"Staged Java 8 LWJGL probe; {len(manifest)} runtime files in manifest")


if __name__ == "__main__":
    main()
