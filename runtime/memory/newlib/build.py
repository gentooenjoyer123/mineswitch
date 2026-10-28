#!/usr/bin/env python3
"""Build a verified project-local newlib allocation-width overlay; never alter the SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def elf_abi(path):
    """Compare allocated bytes, named globals and symbolic relocations, not file metadata."""
    blob = path.read_bytes()
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", blob)
    if header[0][:6] != b"\x7fELF\x02\x01" or header[2] != 183:
        raise ValueError("Expected little-endian ELF64 AArch64 object: " + str(path))
    sections = [struct.unpack_from("<IIQQQQIIQQ", blob, header[6] + i * header[11])
                for i in range(header[12])]
    names_section = sections[header[13]]
    names = blob[names_section[4]:names_section[4] + names_section[5]]

    def string(data, start):
        return data[start:].split(b"\0", 1)[0].decode()

    section_names = [string(names, section[0]) for section in sections]
    symbol_tables = {}
    globals_ = {}
    for index, section in enumerate(sections):
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        strings = blob[strings[4]:strings[4] + strings[5]]
        symbols = []
        for offset in range(section[4], section[4] + section[5], section[9]):
            name, info, other, owner, value, size = struct.unpack_from("<IBBHQQ", blob, offset)
            name = string(strings, name)
            if not name and 0 < owner < len(sections):
                name = section_names[owner]
            symbols.append(name)
            if info >> 4 in (1, 2) and owner:
                globals_[name] = (info, other, section_names[owner], value, size)
        symbol_tables[index] = symbols
    allocated = {}
    for index, section in enumerate(sections):
        if section[2] & 2 and section[5]:
            contents = b"" if section[1] == 8 else blob[section[4]:section[4] + section[5]]
            allocated[section_names[index]] = (section[1], section[2], section[5], section[8], contents)
    relocations = {}
    for section in sections:
        if section[1] != 4 or not sections[section[7]][2] & 2:
            continue
        entries = []
        for offset in range(section[4], section[4] + section[5], section[9]):
            location, info, addend = struct.unpack_from("<QQq", blob, offset)
            entries.append((location, info & 0xffffffff, symbol_tables[section[6]][info >> 32], addend))
        relocations[section_names[section[7]]] = entries
    return allocated, globals_, relocations


def main():
    if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
        os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"), "--",
                                 sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, default=ROOT / ".toolchain/opt/devkitpro/devkitA64")
    parser.add_argument("--output", type=Path, default=HERE / "build/libmineswitch_newlib.a")
    args = parser.parse_args()
    pin = json.loads((HERE / "provenance.json").read_text())
    sdk = args.toolchain.resolve()
    cc = sdk / "bin/aarch64-none-elf-gcc"
    ar = sdk / "bin/aarch64-none-elf-ar"
    libc = sdk / "aarch64-none-elf/lib/pic/libc.a"
    if sha(libc) != pin["installed_sdk"]["libc_sha256"]:
        raise SystemExit("SDK PIC libc differs from the audited archive; re-audit source and allocator ABI")
    version = subprocess.check_output([str(cc), "-dumpfullversion"], text=True).strip()
    if version != pin["installed_sdk"]["gcc_version"]:
        raise SystemExit("Compiler version differs from the audited source-to-SDK comparison")
    for name, digest in pin["sources"].items():
        if sha(HERE / "vendor" / name) != digest:
            raise SystemExit("Pinned vendor source changed: " + name)

    build = HERE / "build"
    original = build / "original"
    patched = build / "patched"
    for directory in (original, patched):
        directory.mkdir(parents=True, exist_ok=True)
    for name in pin["sources"]:
        shutil.copyfile(HERE / "vendor" / name, patched / name)
    run("patch", "--batch", "--forward", "--fuzz=0", "-s", "-p1", "-d", patched,
        "-i", HERE / "allocator-width.patch")
    flags = ["-D_LIBC", "-O2", "-fno-builtin", "-fPIC", "-ffunction-sections", "-fdata-sections", "-Werror"]
    audit = {}
    objects = []
    for name, digest in pin["installed_sdk"]["allocator_members"].items():
        reference = original / ("sdk-" + name + ".o")
        reference.write_bytes(subprocess.check_output([str(ar), "p", str(libc), "libc_a-" + name + ".o"]))
        if sha(reference) != digest:
            raise SystemExit("Unexpected SDK allocator member: " + name)
        unmodified = original / (name + ".o")
        run(cc, *flags, "-c", HERE / "vendor" / (name + ".c"), "-o", unmodified)
        original_abi = elf_abi(unmodified)
        sdk_abi = elf_abi(reference)
        if original_abi != sdk_abi:
            for label, left, right in zip(("allocated sections", "global ABI", "relocations"), original_abi, sdk_abi):
                if left != right:
                    raise SystemExit("Unmodified " + name + " does not match installed SDK " + label)
        target = patched / (name + ".o")
        run(cc, *flags, "-c", patched / (name + ".c"), "-o", target)
        updated_abi = elf_abi(target)
        # Only function bodies/relocations may change. All shared allocator
        # objects retain exact bytes, sizes, alignments and symbolic references.
        shared_data = lambda abi: {key: value for key, value in abi[0].items()
                                  if not key.startswith((".text", ".eh_frame"))}
        data_symbols = lambda abi: {key: value for key, value in abi[1].items() if value[0] & 15 != 2}
        data_relocations = lambda abi: {key: value for key, value in abi[2].items()
                                      if not key.startswith((".text", ".eh_frame"))}
        if (shared_data(updated_abi) != shared_data(sdk_abi) or
                data_symbols(updated_abi) != data_symbols(sdk_abi) or
                data_relocations(updated_abi) != data_relocations(sdk_abi) or
                set(updated_abi[1]) != set(sdk_abi[1])):
            raise SystemExit("Patched allocator changed the SDK shared-data ABI: " + name)
        audit[name] = {"unmodified_matches_sdk": True, "patched_shared_data_matches_sdk": True,
                       "object_sha256": sha(target), "defined_symbols": sorted(updated_abi[1])}
        objects.append(target)
    run(sys.executable, HERE / "tests/run.py", "--source-dir", HERE / "vendor", "--expect-unpatched")
    run(sys.executable, HERE / "tests/run.py", "--source-dir", patched)
    # Read-only observer is a separate member, using the pinned declarations.
    # It must not define allocator state or replace another SDK entrypoint.
    snapshot = build / "free_snapshot.o"
    run(cc, *flags, "-mtp=soft", "-ftls-model=local-exec",
        "-c", HERE / "free_snapshot.c", "-o", snapshot)
    snapshot_symbols = elf_abi(snapshot)[1]
    expected = {"mineswitch_newlib_snapshot", "mineswitch_newlib_allocation_failed",
                "mineswitch_newlib_failure_reset", "mineswitch_newlib_failure_take"}
    if set(snapshot_symbols) != expected or any(v[0] & 15 != 2 for v in snapshot_symbols.values()):
        raise SystemExit("Diagnostic observer exported unexpected allocator state or symbols")
    for target in objects:
        shutil.copyfile(target, build / target.name)
    archive = build / "libmineswitch_newlib.a"
    if archive.exists():
        archive.unlink()
    run(ar, "rcsD", archive, *objects, snapshot)
    if sha(libc) != pin["installed_sdk"]["libc_sha256"]:
        raise SystemExit("Installed SDK changed during verification")
    report = {"sdk_libc_sha256": sha(libc), "patch_sha256": sha(HERE / "allocator-width.patch"),
              "patched_source_sha256": sha(patched / "_mallocr.c"), "audit": audit,
              "host_tests": "passed", "negative_control": "passed", "archive_sha256": sha(archive),
              "free_snapshot_source_sha256": sha(HERE / "free_snapshot.c"),
              "free_snapshot_header_sha256": sha(HERE / "free_snapshot.h"),
              "free_snapshot_object_sha256": sha(snapshot)}
    (build / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output != archive.resolve():
        shutil.copyfile(archive, output)
    print("NEWLIB_OVERLAY_READY path=" + str(output) + " sha256=" + sha(output))


if __name__ == "__main__":
    main()
