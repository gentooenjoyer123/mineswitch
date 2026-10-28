#!/usr/bin/env python3
"""Assemble the Java 8 Horizon runtime ROMFS (a JRE layout) and probe classes.

The Java class library is the pinned OpenJDK 8u492-b09 class files (Temurin
host JRE built from the same jdk8u492-ga tag as .upstream/openjdk8; SHA256 of
each copied file is checked against runtime/java8/jre-8u492-files.json when
present, and recorded otherwise). Native code is NOT copied: the VM and JNI
libraries are linked into the NRO. The Horizon platform classes
(.upstream/openjdk8/jdk/src/horizon/classes) are compiled into lib/horizon.jar,
which the Horizon HotSpot puts first on the boot class path.
Output: runtime/java8/generated-probe/romfs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BOOT = Path(os.environ.get("MINESWITCH_BOOT_JDK8", "/opt/openjdk-bin-8"))
GENERATED = Path(os.environ.get("MINESWITCH_JDK8_GENERATED_DIR", HERE / "generated-probe"))
PIN = HERE / "jre-8u492-files.json"
JRE_FILES = [
    "lib/rt.jar", "lib/jce.jar", "lib/jsse.jar", "lib/charsets.jar", "lib/resources.jar",
    "lib/tzdb.dat", "lib/currency.data", "lib/calendars.properties",
    "lib/hijrah-config-umalqura.properties", "lib/content-types.properties",
    "lib/logging.properties", "lib/net.properties",
    "lib/ext/localedata.jar", "lib/ext/cldrdata.jar", "lib/ext/sunjce_provider.jar",
    "lib/ext/zipfs.jar", "lib/ext/dnsns.jar",
]
JRE_DIRS = ["lib/security", "lib/cmm"]
JDK8 = Path(os.environ.get("MINESWITCH_JDK8_SOURCE", ROOT / ".upstream/openjdk8"))
# Shared upstream classes that the Linux rt.jar omits but Horizon uses.
OVERLAY_SHARED = ["jdk/src/share/classes/sun/nio/fs/PollingWatchService.java"]


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


LAUNCH_OPTIONS = {
    "interpreter.options": ["-Xint"],
    "c1-eager.options": ["-XX:+TieredCompilation", "-XX:TieredStopAtLevel=1",
                         "-XX:Tier3InvocationThreshold=20", "-XX:Tier3MinInvocationThreshold=10",
                         "-XX:Tier3CompileThreshold=50", "-XX:Tier3BackEdgeThreshold=1000",
                         "-XX:+PrintCompilation"],
    "c2-eager.options": ["-XX:-TieredCompilation", "-XX:CompileThreshold=200",
                         "-XX:OnStackReplacePercentage=140", "-XX:+PrintCompilation"],
}


def build_overlay(jre: Path, target: Path, env: dict) -> int:
    sources = sorted((JDK8 / "jdk/src/horizon/classes").rglob("*.java"))
    sources += [JDK8 / name for name in OVERLAY_SHARED]
    classes = HERE / "overlay-build"
    shutil.rmtree(classes, ignore_errors=True)
    classes.mkdir(parents=True)
    subprocess.run([str(BOOT / "bin/javac"), "-source", "8", "-target", "8", "-nowarn",
                    "-XDignore.symbol.file", "-bootclasspath", str(jre / "lib/rt.jar"),
                    "-d", str(classes), *map(str, sources)], check=True, env=env)
    files = sorted(p for p in classes.rglob("*.class"))
    target.parent.mkdir(parents=True, exist_ok=True)
    # Deterministic archive: sorted entries, fixed timestamps, stored.
    with zipfile.ZipFile(target, "w", zipfile.ZIP_STORED) as jar:
        for path in files:
            info = zipfile.ZipInfo(path.relative_to(classes).as_posix(), (1980, 1, 1, 0, 0, 0))
            jar.writestr(info, path.read_bytes())
    shutil.rmtree(classes)
    return len(files)


def main() -> None:
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"), "--",
                                 sys.executable, __file__, *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    jre = BOOT / "jre"
    version = subprocess.run([str(BOOT / "bin/java"), "-version"], capture_output=True, text=True).stderr
    if "1.8.0_492-b09" not in version:
        raise SystemExit(f"Expected the pinned 1.8.0_492-b09 class library, found:\n{version}")
    files = list(JRE_FILES)
    for directory in JRE_DIRS:
        files += sorted(p.relative_to(jre).as_posix() for p in (jre / directory).rglob("*") if p.is_file())
    digests = {name: sha256(jre / name) for name in files}
    if PIN.is_file():
        pinned = json.loads(PIN.read_text())
        mismatch = [n for n, d in digests.items() if pinned.get(n) != d]
        if mismatch:
            raise SystemExit(f"Class library differs from the pin {PIN}: {mismatch[:5]}")
    else:
        PIN.write_text(json.dumps(digests, indent=2, sort_keys=True) + "\n")
        print(f"Recorded class-library pin: {PIN}")
    romfs = GENERATED / "romfs"
    runtime = romfs / "jdk"
    if runtime.exists():
        shutil.rmtree(runtime)
    for name in files:
        target = runtime / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(jre / name, target)
    (runtime / "release").write_text('JAVA_VERSION="1.8.0_492"\nOS_NAME="Horizon"\nOS_ARCH="aarch64"\n'
                                     'IMPLEMENTOR="MineSwitch (OpenJDK 8u492 HotSpot for Horizon)"\n')
    env = dict(os.environ)
    env.pop("_JAVA_OPTIONS", None)
    count = build_overlay(jre, runtime / "lib/horizon.jar", env)
    print(f"Horizon overlay: {count} classes -> {runtime / 'lib/horizon.jar'}")
    # Device triage option sets (docs/JAVA8_PORT.md): interpreter baseline and
    # eager C1/C2 compilation to exercise the RW/RX JIT code paths.
    launch = romfs / "launch"
    launch.mkdir(parents=True, exist_ok=True)
    for name, lines in LAUNCH_OPTIONS.items():
        (launch / name).write_text("\n".join(lines) + "\n")
    tests = romfs / "tests"
    tests.mkdir(parents=True, exist_ok=True)
    build = HERE / "tests/build"
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)
    subprocess.run([str(BOOT / "bin/javac"), "-source", "8", "-target", "8",
                    "-bootclasspath", str(jre / "lib/rt.jar"), "-d", str(build),
                    str(HERE / "tests/HelloHorizon.java")], check=True, env=env)
    shutil.copyfile(build / "HelloHorizon.class", tests / "HelloHorizon.class")
    for extra in build.glob("HelloHorizon$*.class"):
        shutil.copyfile(extra, tests / extra.name)
    entries, total = [], 0
    for path in sorted(runtime.rglob("*")):
        if path.is_file() and path.name != "files.manifest":
            size = path.stat().st_size
            entries.append(f"{sha256(path)} {size} {path.relative_to(runtime).as_posix()}\n")
            total += size
    (runtime / "files.manifest").write_text("".join(entries))
    print(f"Java 8 runtime ROMFS: {len(entries)} files, {total} bytes -> {runtime}")


if __name__ == "__main__":
    main()
