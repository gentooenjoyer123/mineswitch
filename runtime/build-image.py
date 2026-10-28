#!/usr/bin/env python3
"""Build selected target modules and package the native Horizon runtime image."""
from __future__ import annotations
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = Path(os.environ.get("MINESWITCH_JDK_SOURCE", ROOT / ".upstream/openjdk21"))
CONF = os.environ.get("MINESWITCH_JDK_CONF", "horizon-aarch64-zero-release")
BUILD = SOURCE / "build" / CONF
BOOT = Path(os.environ.get("MINESWITCH_BOOT_JDK", "/opt/openjdk-bin-21"))
GENERATED = Path(os.environ.get("MINESWITCH_GENERATED_DIR", ROOT / "runtime/generated")).resolve()

def run(*arguments: object, capture: bool = False) -> str:
    result = subprocess.run([str(arg) for arg in arguments], check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else ""

@contextmanager
def build_lock():
    with (SOURCE / "build/.mineswitch-build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield

def read_roots() -> list[str]:
    if "MINESWITCH_MODULES" in os.environ:
        names = os.environ["MINESWITCH_MODULES"].replace(",", " ").split()
    else:
        profile = Path(os.environ.get("MINESWITCH_MODULES_FILE", ROOT / "runtime/modules-minecraft-1.21.1.txt"))
        names = [line.split("#", 1)[0].strip() for line in profile.read_text().splitlines()]
    roots = sorted({"java.base", *(name for name in names if name)})
    for name in roots:
        if not re.fullmatch(r"[a-z][a-z0-9.]*", name):
            raise ValueError(f"Invalid module name: {name!r}")
    return roots

def ignored(directory: str, names: list[str]) -> list[str]:
    return [name for name in names if name.startswith("_the.") or name.endswith(".marker")]

def copy_tree(source: Path, target: Path) -> None:
    if source.is_dir():
        shutil.copytree(source, target, dirs_exist_ok=True, ignore=ignored)

def spec_value(name: str) -> str:
    match = re.search(r"^\s*" + re.escape(name) + r"\s*:?=\s*(.*)$", (BUILD / "spec.gmk").read_text(), re.M)
    if not match:
        raise ValueError(f"Missing configure variable {name}")
    return match.group(1).strip()

def host_jlink_patch(stage: Path) -> Path:
    sources, classes = stage / "host-patch-src", stage / "host-patch"
    java = sources / "jdk/internal/util/OperatingSystem.java"
    java.parent.mkdir(parents=True)
    text = (SOURCE / "src/java.base/share/classes/jdk/internal/util/OperatingSystem.java").read_text()
    if "PlatformProps.TARGET_OS_IS_HORIZON" not in text:
        raise RuntimeError("Missing Horizon OperatingSystem port")
    java.write_text(text.replace("PlatformProps.TARGET_OS_IS_HORIZON", '"horizon".equals(PlatformProps.CURRENT_OS_STRING)'))
    run(BOOT / "bin/javac", "--patch-module", f"java.base={sources}", "-d", classes, java)
    probe = stage / "CheckHostPlatform.java"
    probe.write_text('''import jdk.internal.util.OperatingSystem;
class CheckHostPlatform {
  public static void main(String[] args) {
    if (OperatingSystem.current().name().equals("HORIZON")) throw new AssertionError("Host loaded target platform constants");
    if (!OperatingSystem.valueOf("HORIZON").name().equals("HORIZON")) throw new AssertionError("Target metadata missing");
    System.out.println("jlink host platform=" + OperatingSystem.current() + "; target metadata=HORIZON");
  }
}
''')
    run(BOOT / "bin/javac", "--add-exports", "java.base/jdk.internal.util=ALL-UNNAMED", "-d", stage, probe)
    run(BOOT / "bin/java", "-Xshare:off", "--patch-module", f"java.base={classes}",
        "--add-exports", "java.base/jdk.internal.util=ALL-UNNAMED", "-cp", stage, "CheckHostPlatform")
    return classes

def create_jmod(stage: Path, module: str, version: str, date: str) -> None:
    part = stage / "modules" / module
    copy_tree(BUILD / "jdk/modules" / module, part / "classes")
    copy_tree(BUILD / "support/modules_conf" / module, part / "conf")
    native_data = BUILD / "support/modules_libs" / module
    for path in sorted(native_data.rglob("*")) if native_data.exists() else []:
        if not path.is_file() or path.suffix in {".a", ".symbols", ".o", ".diz", ".map", ".debuginfo"}:
            continue
        relative = path.relative_to(native_data)
        if any(segment.startswith("_the.") or segment.endswith(".dSYM") for segment in relative.parts):
            continue
        destination = part / "libs" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
    copy_tree(BUILD / "support/modules_legal/common", part / "legal")
    filtered = BUILD / "support/modules_legal" / module
    if filtered.is_dir():
        copy_tree(filtered, part / "legal")
    else:
        for platform in ("share", "unix", "horizon"):
            copy_tree(SOURCE / "src" / module / platform / "legal", part / "legal")
    command: list[object] = [BOOT / "bin/jmod", "create", "--class-path", part / "classes",
        "--target-platform", "horizon-aarch64", "--module-version", version, "--date", date]
    for option, directory in (("--config", "conf"), ("--libs", "libs"), ("--legal-notices", "legal")):
        if (part / directory).is_dir():
            command.extend((option, part / directory))
    run(*command, stage / "jmods" / f"{module}.jmod")

def manifest(image: Path) -> dict[str, object]:
    for name in ("lib/modules", "lib/tzdb.dat", "lib/security/cacerts", "conf/security/java.security"):
        if not (image / name).is_file():
            raise RuntimeError(f"Required runtime file missing: {name}")
    entries, total = [], 0
    for path in sorted(image.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(image).as_posix()
        if relative == "files.manifest":
            continue
        if path.suffix in {".a", ".symbols", ".o"} or relative.startswith("bin/"):
            raise RuntimeError(f"Native build artifact in image: {relative}")
        if any(char in relative for char in "\n\r "):
            raise ValueError(f"Unsupported manifest path: {relative}")
        if path.is_symlink():
            if not path.resolve().is_relative_to(image.resolve()):
                raise ValueError(f"External runtime symlink: {relative}")
            contents = path.read_bytes()
            path.unlink()
            path.write_bytes(contents)
        with path.open("rb") as source:
            sha = hashlib.file_digest(source, "sha256").hexdigest()
        length = path.stat().st_size
        entries.append(f"{sha} {length} {relative}\n")
        total += length
    (image / "files.manifest").write_text("".join(entries))
    print(f"Runtime image: {len(entries)} files; {total} payload bytes")
    return {"manifest_entries": len(entries), "payload_bytes": total}

def replace_directory(source: Path, target: Path, stage: Path) -> None:
    previous = stage / ("old-" + target.name)
    if target.exists():
        target.rename(previous)
    try:
        source.rename(target)
    except BaseException:
        if previous.exists():
            previous.rename(target)
        raise

def main() -> None:
    roots = read_roots()
    GENERATED.mkdir(parents=True, exist_ok=True)
    run(ROOT / "runtime/build-horizon.sh", *(name + "-java" for name in roots))
    with build_lock():
        closure = run(BOOT / "bin/java", ROOT / "runtime/tools/ModuleClosure.java",
                      BUILD / "jdk/modules", ",".join(roots), capture=True).split(",")
    targets = ["hotspot"]
    for name in closure:
        for phase, suffix in (("Copy", "copy"), ("Gendata", "gendata"), ("Lib", "libs-only")):
            if (SOURCE / "make/modules" / name / f"{phase}.gmk").is_file():
                targets.append(f"{name}-{suffix}")
    run(ROOT / "runtime/build-horizon.sh", *targets)
    with build_lock(), tempfile.TemporaryDirectory(prefix=".image-stage.", dir=GENERATED) as temporary:
        stage = Path(temporary)
        (stage / "jmods").mkdir()
        version, date = spec_value("VERSION_NUMBER"), spec_value("SOURCE_DATE_ISO_8601")
        for module in closure:
            create_jmod(stage, module, version, date)
        patch = host_jlink_patch(stage)
        run(BOOT / "bin/jlink", f"-J--patch-module=java.base={patch}", "-J-Xshare:off",
            "--module-path", stage / "jmods", "--add-modules", ",".join(roots),
            "--compress=2", "--endian", "little", "--no-header-files", "--no-man-pages",
            "--output", stage / "jdk")
        statistics = manifest(stage / "jdk")
        module_list = stage / "modules.list"
        module_list.write_text("\n".join(closure) + "\n")
        run(ROOT / "runtime/generate-native-registry.py", "--build-dir", BUILD,
            "--modules-file", module_list, "--output", GENERATED / "native_registry.c", "--lock-held")
        (GENERATED / "romfs").mkdir(exist_ok=True)
        replace_directory(stage / "jdk", GENERATED / "romfs/jdk", stage)
        replace_directory(stage / "jmods", GENERATED / "jmods", stage)
        module_list.replace(GENERATED / "modules.list")
        (GENERATED / "module-image.json").write_text(json.dumps({
            "schema": 2, "platform": "horizon-aarch64", "version": version,
            "build_configuration": CONF,
            "requested_roots": roots, "module_closure": closure, **statistics,
        }, indent=2) + "\n")
        print("Horizon image ready:", GENERATED / "romfs/jdk")

if __name__ == "__main__":
    main()
