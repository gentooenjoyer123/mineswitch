#!/usr/bin/env python3
"""Generate Horizon's built-in native registry from actual target archives."""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
LIBRARIES = ("jvm", "java", "zip", "jimage", "verify", "nio", "net", "syslookup")
PREFIXES = ("Java_", "JNI_", "JVM_", "JDK_", "JNU_", "ZIP_", "JIMAGE_", "JAWT_", "AWT_", "FT_", "j2d_")
EXTRA = {"VerifyClassForMajorVersion", "GetStringPlatformChars", "syslookup", "AWTIsHeadless"}
REQUIRED = {
    "jvm": {"JNI_CreateJavaVM", "JNI_GetCreatedJavaVMs", "JNI_GetDefaultJavaVMInitArgs"},
    "java": {"JNI_OnLoad_java", "JDK_Canonicalize", "JNU_NewStringPlatform", "GetStringPlatformChars"},
    "zip": {"JNI_OnLoad_zip", "ZIP_Open", "ZIP_Close", "ZIP_FindEntry", "ZIP_ReadEntry", "ZIP_CRC32", "ZIP_InflateFully"},
    "jimage": {"JIMAGE_Open", "JIMAGE_Close", "JIMAGE_FindResource", "JIMAGE_GetResource"},
    "verify": {"JNI_OnLoad_verify", "VerifyClassForMajorVersion"},
    "nio": {"JNI_OnLoad_nio"},
    "net": {"JNI_OnLoad_net"},
    "syslookup": {"syslookup"},
}


def symbols(nm: Path, archive: Path, patterns: list[str] | None = None,
            forced: list[str] | None = None) -> list[str]:
    result = subprocess.run(
        [str(nm), "--extern-only", "--defined-only", "--format=posix", str(archive)],
        check=True, text=True, capture_output=True,
    )
    selected = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[1] not in {"T", "W"}:
            continue
        name = fields[0]
        matches = any(re.search(pattern, name) for pattern in patterns) if patterns is not None else name.startswith(PREFIXES) or name in EXTRA
        if matches or name in (forced or []):
            if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
                raise ValueError(f"Unsupported native symbol spelling: {name!r}")
            selected.add(name)
    return sorted(selected)


def library_inputs(args: argparse.Namespace) -> tuple[list[str], list[dict]]:
    modules_file = args.modules_file
    if modules_file is None and (ROOT / "runtime/generated/modules.list").exists():
        modules_file = ROOT / "runtime/generated/modules.list"
    modules = sorted({line.split("#", 1)[0].strip() for line in modules_file.read_text().splitlines()
                      if line.split("#", 1)[0].strip()}) if modules_file else ["java.base"]
    libraries = []
    for module in sorted(modules, key=lambda name: (name != "java.base", name)):
        folder = args.build_dir / "support/modules_libs" / module
        if module == "java.base":
            candidates = sorted(folder.glob("*/libjvm.a"))
            if args.jvm_variant:
                candidates = [path for path in candidates if path.parent.name == args.jvm_variant]
            if len(candidates) != 1:
                raise RuntimeError(f"Expected one JVM archive for {args.jvm_variant or 'auto'}; found {candidates}")
            libraries.append({"name": "jvm", "module": module, "archive": candidates[0]})
        for archive in sorted(folder.glob("lib*.a")):
            if archive.name == "libjli.a":
                continue
            libraries.append({"name": archive.stem[3:], "module": module, "archive": archive})
    specs = args.extra_spec
    if not specs and os.environ.get("MINESWITCH_NATIVE_SPECS", "auto") != "none":
        configured = os.environ.get("MINESWITCH_NATIVE_SPECS")
        if configured and configured != "auto":
            specs = [Path(name) for name in configured.split(os.pathsep)]
        elif (ROOT / "runtime/lwjgl/registry-libraries.json").exists():
            specs = [ROOT / "runtime/lwjgl/registry-libraries.json"]
    for spec in specs:
        for item in json.loads(spec.read_text()):
            archive = Path(item["archive"])
            libraries.append({**item, "module": None,
                              "archive": archive if archive.is_absolute() else ROOT / archive})
    names = [entry["name"] for entry in libraries]
    if len(names) != len(set(names)):
        raise RuntimeError("Duplicate registered library names; use one real archive per library")
    required_base = set(LIBRARIES)
    jvm_variant = next(entry["archive"].parent.name for entry in libraries if entry["name"] == "jvm")
    # Zero needs the real libffi-based foreign linker. Native AArch64 supplies
    # its own VM linkage path and does not build this optional fallback archive.
    if jvm_variant == "zero":
        required_base.add("fallbackLinker")
    missing = required_base - set(names)
    if missing:
        raise RuntimeError(f"Missing base runtime archives: {sorted(missing)}")
    required_modules = {"java.desktop": {"awt", "awt_headless", "fontmanager", "freetype", "javajpeg", "jsound", "lcms", "mlib_image"},
                        "java.management": {"management"}, "jdk.management": {"management_ext"}, "java.prefs": {"prefs"}}
    for module in modules:
        absent = required_modules.get(module, set()) - set(names)
        if absent:
            raise RuntimeError(f"Missing {module} native archives: {sorted(absent)}")
    return modules, libraries


def atomic_write(path: Path, text: str) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    temporary.replace(path)


def generate(args: argparse.Namespace) -> None:
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    modules, libraries = library_inputs(args)
    jvm_variant = next(item["archive"].parent.name for item in libraries if item["name"] == "jvm")
    metadata = {"schema": 3, "jvm_variant": jvm_variant, "modules": modules, "libraries": []}
    lines = [
        "/* Generated from actual Horizon archive function exports; do not edit. */",
        '#include "horizon_bridge.h"',
        "#include <stddef.h>",
        "",
    ]
    tables = []
    ordinal = 0
    for item in libraries:
        library, archive = item["name"], item["archive"]
        if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", library):
            raise ValueError(f"Unsupported library name: {library!r}")
        if not archive.is_file():
            raise FileNotFoundError(archive)
        exported = symbols(args.nm, archive, item.get("symbol_patterns"), item.get("forced_symbols"))
        missing = (REQUIRED.get(library, set()) | set(item.get("required_symbols", [])) |
                   set(item.get("forced_symbols", []))) - set(exported)
        if missing:
            raise RuntimeError(f"{library}: required real exports missing: {', '.join(sorted(missing))}")
        entries = []
        for symbol in exported:
            alias = f"mineswitch_export_{ordinal}"
            ordinal += 1
            lines.append(f'extern void {alias}(void) __asm__({json.dumps(symbol)});')
            entries.append((symbol, alias))
        # A library handle may be used by either built-in or ordinary JNI lookup.
        # Both names resolve to the same real suffixed entry, with no extra code.
        for hook in ("JNI_OnLoad", "JNI_OnUnload"):
            suffixed = hook + "_" + library
            if suffixed in exported and hook not in exported:
                entries.append((hook, next(alias for name, alias in entries if name == suffixed)))
        table = "mineswitch_native_" + library
        if entries:
            lines.append(f"static const struct HorizonNativeSymbol {table}[] = {{")
            for symbol, alias in sorted(entries):
                lines.append(f"    {{{json.dumps(symbol)}, (void *)&{alias}}},")
            lines.append("};")
            tables.append((library, table, f"sizeof({table}) / sizeof({table}[0])"))
        else:
            # Empty libraries may be compiled helper archives; their presence
            # is real, and no callable symbol is invented for the registry.
            tables.append((library, "NULL", "0"))
        lines.append("")
        with archive.open("rb") as archive_file:
            archive_digest = hashlib.file_digest(archive_file, "sha256").hexdigest()
        metadata["libraries"].append({
            "name": library, "module": item["module"], "archive": str(archive.resolve()),
            "archive_sha256": archive_digest,
            "forced_symbols": item.get("forced_symbols", []),
            "symbols": [name for name, _ in sorted(entries)],
        })
    lines.extend(["int mineswitch_register_natives(void) {", "    int status;"])
    for library, table, count in tables:
        lines.append(f"    status = horizon_register_native_library({json.dumps(library)}, {table}, {count});")
        lines.append("    if (status != 0) return status;")
    lines.extend(["    return 0;", "}", ""])
    atomic_write(output, "\n".join(lines))
    atomic_write(output.with_suffix(".json"), json.dumps(metadata, indent=2) + "\n")
    atomic_write(output.parent / "native-archives.list", "".join(str(item["archive"].resolve()) + "\n" for item in libraries))
    make_lines = ["# Generated from the same actual archives as native_registry.c",
                  "MINESWITCH_JDK_BUILD := " + str(args.build_dir.resolve()).replace(" ", "\\ "),
                  "MINESWITCH_JVM_VARIANT := " + jvm_variant]
    for variable, entries in (("MINESWITCH_JDK_ARCHIVES", [i for i in libraries if i["module"]]),
                              ("MINESWITCH_EXTRA_ARCHIVES", [i for i in libraries if not i["module"]])):
        paths = [str(item["archive"].resolve()).replace(" ", "\\ ") for item in entries]
        make_lines.append(variable + " := " + " ".join(paths))
    make_lines.append("MINESWITCH_FORCE_SYMBOLS := " + " ".join(symbol for item in libraries for symbol in item.get("forced_symbols", [])))
    atomic_write(output.parent / "native-archives.mk", "\n".join(make_lines) + "\n")
    print(f"Generated {output}: {ordinal} real function symbols across {len(tables)} libraries")


def main() -> None:
    source = Path(os.environ.get("MINESWITCH_JDK_SOURCE", ROOT / ".upstream/openjdk21"))
    conf = os.environ.get("MINESWITCH_JDK_CONF", "horizon-aarch64-zero-release")
    devkit = Path(os.environ.get("DEVKITPRO", ROOT / ".toolchain/opt/devkitpro"))
    devkit_a64 = Path(os.environ.get("DEVKITA64", devkit / "devkitA64"))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=source / "build" / conf)
    parser.add_argument("--jvm-variant", choices=("zero", "server"),
                        help="Select an actual built VM; inferred when exactly one exists")
    parser.add_argument("--nm", type=Path, default=devkit_a64 / "bin/aarch64-none-elf-nm")
    generated = Path(os.environ.get("MINESWITCH_GENERATED_DIR", ROOT / "runtime/generated"))
    parser.add_argument("--output", type=Path, default=generated / "native_registry.c")
    parser.add_argument("--modules-file", type=Path)
    parser.add_argument("--extra-spec", type=Path, action="append", default=[])
    parser.add_argument("--lock-held", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.lock_held:
        generate(args)
    else:
        with (args.build_dir.parent / ".mineswitch-build.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            generate(args)


if __name__ == "__main__":
    main()
