#!/usr/bin/env python3
"""Build isolated native SHA5 tests; never alter the target image or launcher.

Run through runtime/limit-build.py. The host copy of the two Java source files
forces only their Horizon branches on. The production source/platform identity
is unchanged. JNI registration explicitly binds the *same* C implementation to
that host copy, and counts calls so an accidental Java fallback cannot pass.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
SOURCE = ROOT / ".upstream/openjdk21"
JDK_BUILD = SOURCE / "build/horizon-aarch64-zero-release"
DEVKIT = Path(os.environ.get("DEVKITPRO", ROOT / ".toolchain/opt/devkitpro"))
HOST_JDK = Path(os.environ.get("MINESWITCH_BOOT_JDK", "/opt/openjdk-bin-21"))
BUILD = HERE / "build/native"


def run(arguments):
    print("+ " + shlex.join(map(str, arguments)), flush=True)
    subprocess.run(list(map(str, arguments)), cwd=ROOT, check=True)


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                                "--", sys.executable, str(Path(__file__).resolve()),
                                *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path,
                        default=ROOT / "runtime/launcher/java/HorizonSha5Probe.java")
    parser.add_argument("--class-name", default="HorizonSha5Probe")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--native-only", action="store_true",
                        help="omit the unmodified-host baseline for an additional probe")
    parser.add_argument("probe_args", nargs="*")
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    source = SOURCE / "src/java.base/horizon/native/libjava/SHA5_horizon.c"

    # Reuse the real target libjava compilation flags without writing into its
    # build tree. The new mbedTLS include is the production makefile addition.
    template = JDK_BUILD / "support/native/java.base/libjava/HorizonSeedGenerator.o.cmdline"
    original = shlex.split(template.read_text())
    target = []
    skip = False
    for index, value in enumerate(original):
        if skip:
            skip = False
            continue
        if value in ("-MF", "-o"):
            target.extend([value, str(BUILD / ("SHA5_horizon.d" if value == "-MF"
                                              else "SHA5_horizon.aarch64.o"))])
            skip = True
        elif value.endswith("/HorizonSeedGenerator.c"):
            target.append(str(source))
        elif value.startswith("-frandom-seed="):
            target.append("-frandom-seed=SHA5_horizon.c")
        else:
            target.append(value)
    target.append("-I" + str(DEVKIT / "portlibs/switch/include"))
    run(target)

    patched_sources = BUILD / "patch-source/sun/security/provider"
    patched_sources.mkdir(parents=True, exist_ok=True)
    java_files = []
    for name in ("DigestBase.java", "SHA5.java"):
        original = SOURCE / "src/java.base/share/classes/sun/security/provider" / name
        content = original.read_text()
        assert content.count("OperatingSystem.isHorizon()") == 1
        content = content.replace("import jdk.internal.util.OperatingSystem;\n", "")
        content = content.replace("OperatingSystem.isHorizon()", "true")
        staged = patched_sources / name
        staged.write_text(content)
        java_files.append(staged)
    patch_classes = BUILD / "patch-classes"
    patch_classes.mkdir(exist_ok=True)
    run([HOST_JDK / "bin/javac", "--patch-module", "java.base=" + str(BUILD / "patch-source"),
         "-d", patch_classes, *java_files])

    shared = BUILD / "libsha5-candidate.so"
    run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
         "-I" + str(HOST_JDK / "include"), "-I" + str(HOST_JDK / "include/linux"),
         "-I" + str(SOURCE / "src/java.base/share/native/libjava"),
         "-I" + str(SOURCE / "src/java.base/unix/native/libjava"),
         source, HERE / "host-registration.c", "-L" + str(HOST_JDK / "lib"),
         "-Wl,-rpath," + str(HOST_JDK / "lib"), "-ljava", "-lmbedcrypto", "-o", shared])
    classes = BUILD / "test-classes"
    classes.mkdir(exist_ok=True)
    compile_sources = [HERE / "Sha5HostRunner.java"]
    if args.probe.exists():
        compile_sources.append(args.probe)
    elif not args.build_only:
        parser.error("probe source does not exist: " + str(args.probe))
    run([HOST_JDK / "bin/javac", "-d", classes, *compile_sources])
    if args.build_only:
        print("SHA5_CANDIDATE_BUILD_PASS", flush=True)
        return
    java = [HOST_JDK / "bin/java", "-Xint", "-Xcheck:jni", "-cp", classes]
    if not args.native_only:
        run([*java, args.class_name, *args.probe_args])
    run([*java, "--patch-module", "java.base=" + str(patch_classes),
         "Sha5HostRunner", shared, args.class_name, *args.probe_args])
    print("SHA5_CANDIDATE_HOST_VALIDATION_PASS", flush=True)


if __name__ == "__main__":
    main()
