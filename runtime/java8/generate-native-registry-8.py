#!/usr/bin/env python3
"""Generate the JDK 8 Horizon static native registry from the real archives.

Same output contract as runtime/generate-native-registry.py (Java 21) and
runtime/java17/generate-native-registry-17.py: native_registry.c registers
every exported JNI/JVM/JDK entry of each archive under its library name, and
native-archives.mk lists the archives the NRO must link. `--lwjgl` also
registers the JVM-independent LWJGL/GLFW/EGL/STB/OpenAL archives of the Java 21
port (runtime/lwjgl/registry-libraries.json), exactly as the Java 17 port does.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
JDK8 = Path(os.environ.get("MINESWITCH_JDK8_SOURCE", ROOT / ".upstream/openjdk8"))
HOTSPOT = JDK8 / "build/horizon-aarch64-hotspot/horizon_aarch64_compiler2/product/libjvm.a"
JDKLIB = JDK8 / "build/horizon-aarch64-jdk/lib"
PREFIXES = ("Java_", "JNI_", "JVM_", "JDK_", "JNU_", "ZIP_", "jio_")
EXTRA = {"VerifyClassCodes", "VerifyClassCodesForMajorVersion", "VerifyClass", "VerifyClassForMajorVersion",
         "VerifyClassname", "VerifyFixClassname",
         "NewStringPlatform", "GetStringPlatformChars", "Canonicalize", "JDK_Canonicalize"}
REQUIRED = {
    "jvm": {"JNI_CreateJavaVM", "JNI_GetCreatedJavaVMs", "JNI_GetDefaultJavaVMInitArgs"},
    "java": {"JNI_OnLoad_java", "Java_java_lang_Object_registerNatives", "VerifyClassCodesForMajorVersion"},
    "zip": {"ZIP_Open", "ZIP_Close", "ZIP_FindEntry", "ZIP_ReadEntry", "ZIP_CRC32"},
    "verify": {"VerifyClassForMajorVersion"},
    "management": {"JNI_OnLoad_management"},
}
NM = Path(os.environ.get("DEVKITPRO", ROOT / ".toolchain/opt/devkitpro")) / "devkitA64/bin/aarch64-none-elf-nm"


def exported(archive: Path, patterns: list[str] | None = None, forced: list[str] | None = None) -> list[str]:
    out = subprocess.run([str(NM), "--extern-only", "--defined-only", "--format=posix", str(archive)],
                         check=True, text=True, capture_output=True).stdout
    names = set()
    for line in out.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in {"T", "W"}:
            name = fields[0]
            if patterns is not None:
                selected = any(re.search(pattern, name) for pattern in patterns)
            else:
                selected = name.startswith(PREFIXES) or name in EXTRA
            if selected or name in (forced or []):
                if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
                    raise ValueError(name)
                names.add(name)
    return sorted(names)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "runtime/java8/generated-probe")
    parser.add_argument("--libraries", default="java,zip,verify,nio,net,management",
                        help="JDK archives to register (in addition to jvm); missing optional ones are skipped")
    parser.add_argument("--lwjgl", action="store_true",
                        help="also register runtime/lwjgl/registry-libraries.json (graphics NRO)")
    args = parser.parse_args()
    libraries = [{"name": "jvm", "archive": HOTSPOT, "jdk": True}]
    for name in args.libraries.split(","):
        archive = JDKLIB / f"lib{name}.a"
        if archive.is_file():
            libraries.append({"name": name, "archive": archive, "jdk": True})
        elif name in REQUIRED:
            raise SystemExit(f"Missing required archive {archive}")
    if args.lwjgl:
        for item in json.loads((ROOT / "runtime/lwjgl/registry-libraries.json").read_text()):
            archive = Path(item["archive"])
            libraries.append({**item, "jdk": False, "archive": archive if archive.is_absolute() else ROOT / archive})
    names = [item["name"] for item in libraries]
    if len(names) != len(set(names)):
        raise SystemExit("Duplicate registered library names")
    support = [JDKLIB / "libfdlibm.a"]
    lines = ["/* Generated from actual JDK 8 Horizon archive exports; do not edit. */",
             '#include "horizon_bridge.h"', "#include <stddef.h>", ""]
    tables, meta, ordinal = [], [], 0
    for item in libraries:
        name, archive = item["name"], item["archive"]
        if not archive.is_file():
            raise SystemExit(f"Missing archive {archive}")
        names = exported(archive, item.get("symbol_patterns"), item.get("forced_symbols"))
        missing = (REQUIRED.get(name, set()) | set(item.get("required_symbols", [])) |
                   set(item.get("forced_symbols", []))) - set(names)
        if missing:
            raise SystemExit(f"{name}: required exports missing: {sorted(missing)}")
        entries = []
        for symbol in names:
            alias = f"mineswitch_export_{ordinal}"
            ordinal += 1
            lines.append(f'extern void {alias}(void) __asm__({json.dumps(symbol)});')
            entries.append((symbol, alias))
        for hook in ("JNI_OnLoad", "JNI_OnUnload"):
            suffixed = f"{hook}_{name}"
            if suffixed in names and hook not in names:
                entries.append((hook, next(a for s, a in entries if s == suffixed)))
        table = f"mineswitch_native_{name}"
        if entries:
            lines.append(f"static const struct HorizonNativeSymbol {table}[] = {{")
            for symbol, alias in sorted(entries):
                lines.append(f"    {{{json.dumps(symbol)}, (void *)&{alias}}},")
            lines.append("};")
            tables.append((name, table, f"sizeof({table}) / sizeof({table}[0])"))
        else:
            tables.append((name, "NULL", "0"))
        lines.append("")
        meta.append({"name": name, "archive": str(archive.resolve()),
                     "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                     "symbols": len(entries)})
    lines += ["int mineswitch_register_natives(void) {", "    int status;"]
    for name, table, count in tables:
        lines.append(f'    status = horizon_register_native_library("{name}", {table}, {count});')
        lines.append("    if (status != 0) return status;")
    lines += ["    return 0;", "}", ""]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "native_registry.c").write_text("\n".join(lines))
    (args.output_dir / "native_registry.json").write_text(json.dumps({"schema": 1, "java": 8, "libraries": meta}, indent=2) + "\n")
    mk = ["# Generated with native_registry.c from the same JDK 8 archives",
          "MINESWITCH_JDK_BUILD := " + str(JDK8 / "build"),
          "MINESWITCH_JVM_VARIANT := server",
          "MINESWITCH_JDK_ARCHIVES := " + " ".join(str(i["archive"].resolve()) for i in libraries if i["jdk"]),
          "MINESWITCH_EXTRA_ARCHIVES := " + " ".join(str(i["archive"].resolve()) for i in libraries if not i["jdk"]),
          "MINESWITCH_FORCE_SYMBOLS := " + " ".join(sym for i in libraries for sym in i.get("forced_symbols", [])),
          "MINESWITCH_SUPPORT_ARCHIVES := " + " ".join(str(p.resolve()) for p in support if p.is_file())]
    (args.output_dir / "native-archives.mk").write_text("\n".join(mk) + "\n")
    print(f"JDK 8 registry: {ordinal} functions across {len(libraries)} libraries -> {args.output_dir}")


if __name__ == "__main__":
    main()
