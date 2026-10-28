#!/usr/bin/env python3
"""Stage Java 17 device probes into the Java 17 probe ROMFS (no device access).

* HelloHorizon (Java 17 variant of runtime/tests/HelloHorizon.java) -> romfs/tests
* WhiteBox JIT and native-contract probes (sources shared with Java 21 in
  runtime/tests/mineswitch/probe, compiled with --release 17 against the
  JDK 17 WhiteBox API) -> romfs/jdk/mineswitch + romfs/launch/*.options
* --host-check runs the same JIT/contract probes on the host JDK 17 HotSpot.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BUILD = HERE / "build"
BOOT = Path(os.environ.get("MINESWITCH_BOOT_JDK17", "/opt/openjdk-bin-17"))
GENERATED = Path(os.environ.get("MINESWITCH_JDK17_GENERATED_DIR", ROOT / "runtime/java17/generated-probe"))
SD_APP = "/switch/mineswitch/runtime-17/mineswitch"
PROBES = ("JitProbe", "StackDepthProbe", "NativeContractProbe")


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def probe_options(level, bootjar):
    return ["-XX:+UnlockDiagnosticVMOptions", "-XX:+WhiteBoxAPI",
            "-Xbootclasspath/a:" + str(bootjar), "-XX:CompileThreshold=1000000",
            "-XX:+TieredCompilation", f"-XX:TieredStopAtLevel={level}",
            "-XX:ReservedCodeCacheSize=32m", "-XX:InitialCodeCacheSize=2m",
            "-Xlog:startuptime=info,os+thread=debug",
            "-XX:+PrintCompilation",
            "-XX:CompileCommand=dontinline,mineswitch.probe.JitProbe::*"]


def java17_probe_sources(target):
    """Copy the shared probe sources, replacing the JDK 19+ Thread.threadId()."""
    target.mkdir(parents=True, exist_ok=True)
    result = []
    for name in PROBES:
        text = (ROOT / "runtime/tests/mineswitch/probe" / f"{name}.java").read_text()
        text = re.sub(r"\.threadId\(\)", ".getId()", text)
        path = target / f"{name}.java"
        path.write_text(text)
        result.append(path)
    return result


def write_manifest(runtime):
    manifest = []
    for path in sorted(runtime.rglob("*")):
        if path.is_file() and path.name != "files.manifest":
            with path.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            manifest.append(f"{digest} {path.stat().st_size} {path.relative_to(runtime).as_posix()}\n")
    (runtime / "files.manifest").write_text("".join(manifest))
    return len(manifest)


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                                 "--", sys.executable, __file__, *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host-check", action="store_true",
                        help="Run the JIT/contract probes on host HotSpot 17, without staging")
    args = parser.parse_args()
    whitebox = ROOT / ".upstream/openjdk17/test/lib/jdk/test/whitebox"
    wb_classes, app_classes, hello = BUILD / "whitebox-classes", BUILD / "app-classes", BUILD / "hello"
    for folder in (wb_classes, app_classes, hello):
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True)
    run(BOOT / "bin/javac", "--release", "17", "-d", wb_classes,
        whitebox / "WhiteBox.java", whitebox / "parser/DiagnosticCommand.java")
    run(BOOT / "bin/javac", "--release", "17", "-cp", wb_classes, "-d", app_classes,
        *java17_probe_sources(BUILD / "probe-src"))
    run(BOOT / "bin/javac", "--release", "17", "-d", hello, HERE / "HelloHorizon.java")
    bootjar, appjar = BUILD / "whitebox.jar", BUILD / "jit-probes.jar"
    run(BOOT / "bin/jar", "--create", "--file", bootjar, "-C", wb_classes, ".")
    run(BOOT / "bin/jar", "--create", "--file", appjar, "-C", app_classes, ".")
    if args.host_check:
        for level in (1, 4):
            for probe in ("mineswitch.probe.JitProbe", "mineswitch.probe.NativeContractProbe"):
                run(BOOT / "bin/java", "-Xms32m", "-Xmx64m", "-XX:+UseSerialGC",
                    *probe_options(level, bootjar), "-cp", appjar, probe, level)
        return
    romfs = GENERATED / "romfs"
    runtime = romfs / "jdk"
    if not (runtime / "lib/modules").is_file():
        raise SystemExit("Build the Java 17 runtime image first (runtime/java17/build-image-17.sh)")
    tests = romfs / "tests"
    tests.mkdir(exist_ok=True)
    shutil.copyfile(hello / "HelloHorizon.class", tests / "HelloHorizon.class")
    app = runtime / "mineswitch"
    app.mkdir(exist_ok=True)
    shutil.copyfile(bootjar, app / bootjar.name)
    shutil.copyfile(appjar, app / appjar.name)
    launch = romfs / "launch"
    launch.mkdir(exist_ok=True)
    for level in (1, 4):
        options = ["-Djava.class.path=" + SD_APP + "/jit-probes.jar",
                   *probe_options(level, SD_APP + "/whitebox.jar")]
        (launch / f"jit-level{level}.options").write_text("\n".join(options) + "\n")
    count = write_manifest(runtime)
    print(f"Staged Java 17 probes: HelloHorizon + JIT probes; {count} runtime files in manifest")


if __name__ == "__main__":
    main()
