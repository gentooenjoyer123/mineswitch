#!/usr/bin/env python3
"""Build the pinned upstream libffi for devkitA64, without installing globally."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request


VERSION = "3.4.8"
ARCHIVE_NAME = f"libffi-{VERSION}.tar.gz"
URL = f"https://github.com/libffi/libffi/releases/download/v{VERSION}/{ARCHIVE_NAME}"
SHA256 = "bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b"
CFLAGS = "-O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec"


def sha256_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def ensure_archive(archive, previous_archive):
    if archive.is_file() and sha256_file(archive) == SHA256:
        return
    partial = archive.with_suffix(".partial")
    try:
        if previous_archive.is_file() and sha256_file(previous_archive) == SHA256:
            shutil.copyfile(previous_archive, partial)
        else:
            print(f"Downloading official libffi {VERSION}...", flush=True)
            with urllib.request.urlopen(URL, timeout=60) as response:
                with partial.open("wb") as output:
                    shutil.copyfileobj(response, output)
        if sha256_file(partial) != SHA256:
            raise SystemExit("libffi archive SHA256 mismatch; refusing to extract.")
        partial.replace(archive)
    finally:
        partial.unlink(missing_ok=True)


def run_logged(arguments, cwd, env, log_path):
    with log_path.open("w") as output:
        result = subprocess.run(
            arguments, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT,
            check=False,
        )
    if result.returncode:
        raise SystemExit(f"libffi build failed ({result.returncode}); see {log_path}")


def main():
    root = Path(__file__).resolve().parent.parent
    work = root / ".research" / "vm-probe-libffi"
    source = work / f"libffi-{VERSION}"
    build = work / "build"
    stage = work / "stage"
    stamp = work / "build-manifest.json"
    devkitpro = Path(os.environ.get("DEVKITPRO", root / ".toolchain/opt/devkitpro")).resolve()
    devkita64 = Path(os.environ.get("DEVKITA64", devkitpro / "devkitA64")).resolve()
    specs = devkitpro / "libnx/switch.specs"
    libnx = devkitpro / "libnx/lib/libnx.a"
    if not specs.is_file() or not libnx.is_file():
        raise SystemExit(f"libnx is missing under {devkitpro}; run scripts/bootstrap-toolchain.py.")

    env = os.environ.copy()
    env.update({
        "DEVKITPRO": str(devkitpro),
        "DEVKITA64": str(devkita64),
        "PATH": f"{devkitpro}/tools/bin:{devkita64}/bin:{env.get('PATH', '')}",
        "CC": "aarch64-none-elf-gcc",
        "CXX": "aarch64-none-elf-g++",
        "AR": "aarch64-none-elf-ar",
        "RANLIB": "aarch64-none-elf-ranlib",
        "CFLAGS": CFLAGS,
        "CXXFLAGS": CFLAGS,
        "CPPFLAGS": "",
        "LDFLAGS": f"-specs={specs} -L{devkitpro}/libnx/lib",
        "LIBS": "-lnx",
        "CONFIG_SITE": "/dev/null",
        "LC_ALL": "C",
    })
    configure_args = [
        str(source / "configure"),
        "--host=aarch64-none-elf",
        "--disable-shared",
        "--enable-static",
        "--disable-docs",
        "--disable-multi-os-directory",
        "--disable-exec-static-tramp",
        f"--prefix={stage}",
    ]
    tool_versions = {}
    for variable in ("CC", "CXX", "AR", "RANLIB"):
        executable = shutil.which(env[variable], path=env["PATH"])
        if executable is None:
            raise SystemExit(f"Missing {env[variable]}; set DEVKITPRO and DEVKITA64.")
        version = subprocess.run(
            [executable, "--version"], env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True,
        ).stdout
        tool_versions[variable] = {"path": executable, "version": version}

    configuration = {
        "source_url": URL,
        "source_sha256": SHA256,
        "configure_args": configure_args,
        "tools": tool_versions,
        "environment": {key: env[key] for key in (
            "CC", "CXX", "AR", "RANLIB", "CFLAGS", "CXXFLAGS", "CPPFLAGS",
            "LDFLAGS", "LIBS", "CONFIG_SITE", "DEVKITPRO", "DEVKITA64",
        )},
        "switch_specs_sha256": sha256_file(specs),
        "libnx_sha256": sha256_file(libnx),
        "bootstrap_sha256": sha256_file(Path(__file__).resolve()),
    }
    artifacts = [stage / "include/ffi.h", stage / "include/ffitarget.h", stage / "lib/libffi.a"]
    if stamp.is_file() and all(path.is_file() for path in artifacts):
        try:
            cached = json.loads(stamp.read_text())
        except (ValueError, OSError):
            cached = {}
        if cached.get("configuration") == configuration and cached.get("artifacts") == {
            str(path.relative_to(stage)): sha256_file(path) for path in artifacts
        }:
            print(f"libffi {VERSION} cached: {stage}", flush=True)
            return

    work.mkdir(parents=True, exist_ok=True)
    archive = work / ARCHIVE_NAME
    ensure_archive(archive, root / ".research/libffi" / ARCHIVE_NAME)
    stamp.unlink(missing_ok=True)
    for directory in (source, build, stage):
        if directory.exists():
            shutil.rmtree(directory)
    with tarfile.open(archive, "r:gz") as bundle:
        members = bundle.getmembers()
        if any(Path(member.name).parts[0] != source.name for member in members):
            raise SystemExit("Unexpected libffi archive root; refusing to extract.")
        bundle.extractall(work, members=members, filter="data")
    build.mkdir()
    print(f"Building libffi {VERSION}; logs: {work}", flush=True)
    run_logged(configure_args, build, env, work / "configure.log")
    run_logged(["make", f"-j{min(os.cpu_count() or 1, 6)}"], build, env, work / "build.log")
    run_logged(["make", "install"], build, env, work / "install.log")
    if not all(path.is_file() for path in artifacts):
        raise SystemExit(f"libffi install is incomplete; see {work / 'install.log'}")
    manifest = {
        "configuration": configuration,
        "artifacts": {str(path.relative_to(stage)): sha256_file(path) for path in artifacts},
    }
    temporary_stamp = stamp.with_suffix(".partial")
    temporary_stamp.write_text(json.dumps(manifest, indent=2) + "\n")
    temporary_stamp.replace(stamp)
    print(f"libffi {VERSION} ready: {stage}", flush=True)


if __name__ == "__main__":
    main()
