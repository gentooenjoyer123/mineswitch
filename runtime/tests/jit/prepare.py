#!/usr/bin/env python3
"""Build diagnostic-only upstream WhiteBox and genuine compiled-method probes."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(__file__).resolve().parent / "build"
BOOT = Path(os.environ.get("MINESWITCH_BOOT_JDK", "/opt/openjdk-bin-21"))
GENERATED = Path(os.environ.get("MINESWITCH_GENERATED_DIR", ROOT / "runtime/generated-jit"))
SD_APP = "/switch/mineswitch/runtime/mineswitch"


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def probe_options(level, bootjar):
    return ["-XX:+UnlockDiagnosticVMOptions", "-XX:+WhiteBoxAPI",
            "-Xbootclasspath/a:" + str(bootjar), "-XX:CompileThreshold=1000000",
            "-XX:+TieredCompilation", f"-XX:TieredStopAtLevel={level}",
            "-XX:ReservedCodeCacheSize=32m", "-XX:InitialCodeCacheSize=2m",
            "-Xlog:startuptime=info,stubs=info,os+thread=debug",
            "-XX:+PrintCompilation",
            "-XX:CompileCommand=dontinline,mineswitch.probe.JitProbe::*"]


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                                 "--", sys.executable, __file__, *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-check", action="store_true",
                        help="Run the same nmethod/semantic checks on host HotSpot, without staging")
    args = parser.parse_args()
    whitebox = ROOT / ".upstream/openjdk21/test/lib/jdk/test/whitebox"
    wb_classes, app_classes = BUILD / "whitebox-classes", BUILD / "app-classes"
    for folder in (wb_classes, app_classes):
        folder.mkdir(parents=True, exist_ok=True)
    run(BOOT / "bin/javac", "--release", "21", "-d", wb_classes,
        whitebox / "WhiteBox.java", whitebox / "parser/DiagnosticCommand.java")
    run(BOOT / "bin/javac", "--release", "21", "-cp", wb_classes, "-d", app_classes,
        ROOT / "runtime/tests/mineswitch/probe/JitProbe.java",
        ROOT / "runtime/tests/mineswitch/probe/StackDepthProbe.java",
        ROOT / "runtime/tests/mineswitch/probe/NativeContractProbe.java")
    bootjar, appjar = BUILD / "whitebox.jar", BUILD / "jit-probes.jar"
    run(BOOT / "bin/jar", "--create", "--file", bootjar, "-C", wb_classes, ".")
    run(BOOT / "bin/jar", "--create", "--file", appjar, "-C", app_classes, ".")
    if args.host_check:
        for level in (1, 4):
            run(BOOT / "bin/java", "-Xms32m", "-Xmx64m", "-XX:+UseSerialGC",
                *probe_options(level, bootjar), "-cp", appjar,
                "mineswitch.probe.JitProbe", level)
            run(BOOT / "bin/java", "-Xms32m", "-Xmx64m", "-XX:+UseSerialGC",
                *probe_options(level, bootjar), "-cp", appjar,
                "mineswitch.probe.NativeContractProbe", level)
        return
    runtime = GENERATED / "romfs/jdk"
    if not (runtime / "lib/modules").is_file():
        raise SystemExit("Build the separate JIT runtime image first, then stage the probes")
    app = runtime / "mineswitch"
    app.mkdir(exist_ok=True)
    shutil.copyfile(bootjar, app / bootjar.name)
    shutil.copyfile(appjar, app / appjar.name)
    launch = GENERATED / "romfs/launch"
    launch.mkdir(exist_ok=True)
    for level in (1, 4):
        options = ["-Djava.class.path=" + SD_APP + "/jit-probes.jar",
                   *probe_options(level, SD_APP + "/whitebox.jar")]
        (launch / f"jit-level{level}.options").write_text("\n".join(options) + "\n")
    manifest = []
    for path in sorted(runtime.rglob("*")):
        if path.is_file() and path.name != "files.manifest":
            with path.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            manifest.append(f"{digest} {path.stat().st_size} {path.relative_to(runtime).as_posix()}\n")
    (runtime / "files.manifest").write_text("".join(manifest))
    print(f"Staged diagnostic-only JIT probes: {len(manifest)} runtime files")


if __name__ == "__main__":
    main()
