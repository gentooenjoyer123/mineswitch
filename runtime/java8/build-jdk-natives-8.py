#!/usr/bin/env python3
"""Cross-compile the JDK 8u core native libraries for Horizon as static archives.

The JDK 8 top-level build has no static-library or Horizon support; these
libraries are compiled from the pinned openjdk8 tree with the same source sets
as jdk/make/lib/*.gmk (Linux selection), Horizon overrides from
jdk/src/horizon/native/<lib>/ (a same-named file replaces the upstream one),
and JNI headers generated from the pinned 8u492 rt.jar (tools/gen-jni-headers-8.py).
Archives land in .upstream/openjdk8/build/horizon-aarch64-jdk/lib/.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
JDK8 = Path(os.environ.get("MINESWITCH_JDK8_SOURCE", ROOT / ".upstream/openjdk8"))
JDK = JDK8 / "jdk"
SRC = JDK / "src"
OUT = Path(os.environ.get("MINESWITCH_JDK8_JDK_OUT", JDK8 / "build/horizon-aarch64-jdk"))
DEVKITPRO = Path(os.environ.get("DEVKITPRO", ROOT / ".toolchain/opt/devkitpro"))
TOOLS = DEVKITPRO / "devkitA64/bin/aarch64-none-elf-"
RUNTIME = ROOT / "runtime"

COMMON_FLAGS = [
    "-march=armv8-a", "-mtune=cortex-a57", "-mtp=soft", "-fPIC", "-ftls-model=local-exec",
    "-O2", "-g", "-std=gnu99", "-fno-strict-aliasing", "-fno-delete-null-pointer-checks",
    "-D__SWITCH__", "-DHORIZON", "-D_GNU_SOURCE", "-D_REENTRANT", "-DNDEBUG",
    "-DARCH=\"aarch64\"", "-DARCHPROPNAME=\"aarch64\"", "-DRELEASE=\"1.8.0_492\"",
    "-DJDK_MAJOR_VERSION=\"1\"", "-DJDK_MINOR_VERSION=\"8\"", "-DJDK_MICRO_VERSION=\"0\"",
    "-DJDK_BUILD_NUMBER=\"b09\"", "-DJDK_UPDATE_VERSION=\"492\"",
    "-isystem", str(DEVKITPRO / "libnx/include"),
    f"-I{OUT / 'headers'}",
    f"-I{SRC / 'share/javavm/export'}", f"-I{SRC / 'solaris/javavm/export'}",
    f"-I{SRC / 'horizon/native/common'}",
    f"-I{SRC / 'share/native/common'}", f"-I{SRC / 'solaris/native/common'}",
    f"-I{RUNTIME / 'io'}", f"-I{RUNTIME / 'memory'}",
    f"-I{JDK8 / 'hotspot/src/os/horizon/vm'}",
]


def dirs(*names: str) -> list[Path]:
    return [SRC / name for name in names]


LIBRARIES = {
    "fdlibm": {
        "dirs": dirs("share/native/java/lang/fdlibm/src"),
        "cflags": [f"-I{SRC / 'share/native/java/lang/fdlibm/include'}"],
    },
    "verify": {
        "dirs": dirs("share/native/common"),
        "include": ["check_code.c", "check_format.c"],
    },
    "java": {
        "dirs": dirs("solaris/native/java/lang", "share/native/java/lang", "share/native/java/lang/reflect",
                     "share/native/java/io", "solaris/native/java/io", "share/native/java/nio",
                     "share/native/java/security", "share/native/common", "share/native/sun/misc",
                     "share/native/sun/reflect", "share/native/java/util",
                     "share/native/java/util/concurrent/atomic", "solaris/native/common",
                     "solaris/native/java/util"),
        "extra_dirs": [RUNTIME / "io"],
        "exclude": ["check_code.c", "check_format.c", "jspawnhelper.c", "java_props_macosx.c",
                    "ProcessImpl_md.c", "WinNTFileSystem_md.c", "dirent_md.c", "WindowsPreferences.c",
                    "childproc.c"],
        "cflags": [f"-I{SRC / 'share/native/java/lang/fdlibm/include'}",
                   f"-I{SRC / 'share/native/java/lang'}", f"-I{SRC / 'solaris/native/java/lang'}",
                   f"-I{SRC / 'share/native/java/io'}", f"-I{SRC / 'solaris/native/java/io'}",
                   f"-I{SRC / 'share/native/java/util'}", f"-I{SRC / 'solaris/native/java/util'}"],
    },
    "zip": {
        "dirs": dirs("share/native/java/util/zip", "share/native/java/util/zip/zlib"),
        "cflags": [f"-I{SRC / 'share/native/java/util/zip'}", f"-I{SRC / 'share/native/java/util/zip/zlib'}",
                   f"-I{SRC / 'share/native/java/io'}", f"-I{SRC / 'solaris/native/java/io'}",
                   "-DHAVE_UNISTD_H=1", "-DHAVE_STDARG_H=1", "-UDEBUG"],
    },
    "net": {
        "dirs": dirs("share/native/java/net", "solaris/native/java/net", "solaris/native/sun/net",
                     "solaris/native/sun/net/dns", "solaris/native/sun/net/spi", "solaris/native/sun/net/sdp"),
        "exclude": ["linux_close.c", "bsd_close.c", "TwoStacksPlainSocketImpl.c", "DualStackPlainSocketImpl.c",
                    "TwoStacksPlainDatagramSocketImpl.c", "DualStackPlainDatagramSocketImpl.c",
                    "NTLMAuthSequence.c", "NetworkInterface_winXP.c", "SdpSupport.c"],
        "cflags": [f"-I{SRC / 'share/native/java/net'}", f"-I{SRC / 'solaris/native/java/net'}"],
    },
    "nio": {
        "dirs": dirs("solaris/native/java/nio", "solaris/native/sun/nio/ch", "solaris/native/sun/nio/fs"),
        "include": ["DatagramChannelImpl.c", "DatagramDispatcher.c", "FileChannelImpl.c", "FileDispatcherImpl.c",
                    "FileKey.c", "IOUtil.c", "MappedByteBuffer.c", "Net.c", "ServerSocketChannelImpl.c",
                    "SocketChannelImpl.c", "SocketDispatcher.c", "InheritedChannel.c", "NativeThread.c",
                    "PollArrayWrapper.c", "UnixCopyFile.c", "UnixNativeDispatcher.c",
                    # Horizon-only sources in jdk/src/horizon/native/nio:
                    "horizon_socket_io.c"],
        "cflags": [f"-I{SRC / 'share/native/sun/nio/ch'}", f"-I{SRC / 'solaris/native/sun/nio/ch'}",
                   f"-I{SRC / 'share/native/java/io'}", f"-I{SRC / 'solaris/native/java/io'}",
                   f"-I{SRC / 'share/native/java/net'}", f"-I{SRC / 'solaris/native/java/net'}"],
    },
    "management": {
        # java.lang.management / sun.management (RuntimeMXBean etc.; Minecraft's
        # snooper and crash reports use it). Same set as ServiceabilityLibraries.gmk.
        "dirs": dirs("share/native/sun/management", "solaris/native/sun/management"),
        "exclude": ["SolarisOperatingSystem.c", "LinuxOperatingSystem.c", "MacosxOperatingSystem.c"],
        "cflags": [f"-I{SRC / 'share/native/sun/management'}"],
    },
}


def sources(name: str, spec: dict) -> list[Path]:
    found: dict[str, Path] = {}
    search = list(spec.get("dirs", [])) + list(spec.get("extra_dirs", []))
    for directory in search:
        for path in sorted(directory.glob("*.c")):
            found.setdefault(path.name, path)
    override = SRC / "horizon/native" / name
    if override.is_dir():
        for path in sorted(override.glob("*.c")):
            found[path.name] = path
    include = spec.get("include")
    if include is not None:
        found = {k: v for k, v in found.items() if k in include}
    for excluded in spec.get("exclude", []):
        found.pop(excluded, None)
    return [found[k] for k in sorted(found)]


def compile_one(source: Path, obj: Path, flags: list[str]) -> tuple[Path, int, str]:
    obj.parent.mkdir(parents=True, exist_ok=True)
    command = [f"{TOOLS}gcc", *flags, "-c", str(source), "-o", str(obj)]
    result = subprocess.run(command, capture_output=True, text=True)
    return source, result.returncode, result.stderr


def build(name: str, jobs: int, syntax_only: bool) -> bool:
    spec = LIBRARIES[name]
    # Static JNI libraries (JEP 178): each library's JNI_OnLoad becomes
    # JNI_OnLoad_<lib>, so several libraries can share one executable.
    flags = COMMON_FLAGS + spec.get("cflags", []) + [f"-DJNI_OnLoad=JNI_OnLoad_{name}",
                                                     f"-DJNI_OnUnload=JNI_OnUnload_{name}"]
    if (SRC / "horizon/native" / name).is_dir():
        flags = flags + [f"-I{SRC / 'horizon/native' / name}"]
    for directory in spec.get("dirs", []):
        flags.append(f"-I{directory}")
    files = sources(name, spec)
    objdir = OUT / "objs" / f"lib{name}"
    ok = True
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(compile_one, f, objdir / (f.stem + ".o"),
                               flags + (["-fsyntax-only"] if syntax_only else [])) for f in files]
        for future in futures:
            source, code, err = future.result()
            if code != 0:
                ok = False
                print(f"FAILED {name}: {source}", flush=True)
                print(err, flush=True)
            elif err.strip() and os.environ.get("MINESWITCH_SHOW_WARNINGS"):
                print(err)
    if ok and not syntax_only:
        lib = OUT / "lib" / f"lib{name}.a"
        lib.parent.mkdir(parents=True, exist_ok=True)
        lib.unlink(missing_ok=True)
        objs = [str(objdir / (f.stem + ".o")) for f in files]
        subprocess.run([f"{TOOLS}ar", "rcs", str(lib), *objs], check=True)
        print(f"lib{name}.a: {len(files)} objects", flush=True)
    return ok


def main() -> None:
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"), "--",
                                 sys.executable, __file__, *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("libraries", nargs="*", default=list(LIBRARIES))
    parser.add_argument("--syntax-only", action="store_true")
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("MINESWITCH_JOBS", "6")))
    args = parser.parse_args()
    if not (OUT / "headers/java_lang_Object.h").is_file():
        raise SystemExit("Generate JNI headers first: runtime/java8/tools/gen-jni-headers-8.py")
    results = {name: build(name, max(1, min(args.jobs, 6)), args.syntax_only) for name in args.libraries}
    failed = [n for n, ok in results.items() if not ok]
    if failed:
        raise SystemExit(f"Failed libraries: {' '.join(failed)}")


if __name__ == "__main__":
    main()
