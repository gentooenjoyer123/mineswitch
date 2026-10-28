#!/usr/bin/env python3
"""Decode a bounded native-allocation profile using its archived matching ELF."""
import argparse
from pathlib import Path
import re
import subprocess
import json
import struct

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log", type=Path)
parser.add_argument("--output", type=Path)
args = parser.parse_args()
log = args.log.read_text(errors="replace")
elf = args.log.with_suffix(".elf")
toolchain = root / ".toolchain/opt/devkitpro/devkitA64/bin"
symbols = subprocess.check_output([str(toolchain / "aarch64-none-elf-nm"), "-n", str(elf)], text=True)
symbol = re.search(r"^([0-9a-f]+) [Tt] allocator_trace_snapshot$", symbols, re.M)
reporter = re.search(r"MINESWITCH_ALLOC_TRACE reporter=(0x[0-9a-f]+)", log)
if not symbol or not reporter:
    raise SystemExit("No complete allocator trace anchor in the log/ELF yet")
base = int(reporter[1], 16) - int(symbol[1], 16)
all_callers = sorted({int(x, 16) for x in re.findall(r"caller=(0x[0-9a-f]+)", log)})
with elf.open("rb") as stream:
    header = stream.read(64)
    phoff = struct.unpack_from("<Q", header, 32)[0]
    phsize, phcount = struct.unpack_from("<HH", header, 54)
    executable = []
    for i in range(phcount):
        stream.seek(phoff+i*phsize)
        kind, flags, _, vaddr, _, _, size, _ = struct.unpack("<IIQQQQQQ", stream.read(56))
        if kind == 1 and flags & 1:
            executable.append((vaddr, vaddr+size))
callers = [x for x in all_callers if any(lo <= x-base-4 < hi for lo,hi in executable)]
if not callers:
    raise SystemExit("No allocation caller records yet")
decoded = subprocess.check_output([str(toolchain / "aarch64-none-elf-addr2line"), "-f", "-C", "-e", str(elf)] +
    [hex(x - base - 4) for x in callers], text=True).splitlines()
lookup = {x: {"function": decoded[2*i], "source": decoded[2*i+1], "elf_pc": hex(x-base-4)} for i,x in enumerate(callers)}
for x in all_callers:
    if x not in lookup:
        lookup[x] = {"function": "<JIT or external mapping>", "source": "not in NRO executable segments", "runtime_pc": hex(x)}
samples, events, regions = [], [], []
free_blocks = []
free_by_id = {}
reclaims = []
mimalloc_samples = []
gpu_alias_samples = []
large_page_samples = []
elapsed = 0
native = {}
for line in log.splitlines():
    heartbeat = re.search(r"JVM_HEARTBEAT elapsed_s=(\d+)", line)
    if heartbeat:
        elapsed = int(heartbeat[1])
    if "MINESWITCH_NATIVE_MEMORY " in line:
        native = {k: int(v) for k,v in re.findall(r"(\w+)=(\d+)", line)}
    if "MINESWITCH_GPU_ALIAS " in line:
        values = {k: int(v, 16) if v.startswith('0x') else int(v)
                  for k,v in re.findall(r"(\w+)=(0x[0-9a-fA-F]+|\d+)", line)}
        gpu_alias_samples.append({"elapsed_s": elapsed, **values})
    elif "MINESWITCH_LARGE_PAGES " in line:
        values = {k: int(v) for k,v in re.findall(r"(\w+)=(\d+)", line)}
        large_page_samples.append({"elapsed_s": elapsed, **values})
    elif "MINESWITCH_MIMALLOC " in line:
        values = {k: int(v) for k,v in re.findall(r"(\w+)=(\d+)", line)}
        mimalloc_samples.append({"elapsed_s": elapsed, **values})
    elif "MINESWITCH_NATIVE_RECLAIM " in line:
        values = {k: int(v) for k,v in re.findall(r"(\w+)=(\d+)", line)}
        reclaims.append({"elapsed_s": elapsed, **values})
    elif "MINESWITCH_FREE_BLOCKS " in line:
        values = dict(re.findall(r"(\w+)=([a-z_]+|\d+)", line))
        row = {key: int(value) if value.isdigit() else value for key,value in values.items()}
        row["elapsed_s"] = elapsed
        free_blocks.append(row)
        free_by_id[row["id"]] = row
    elif "MINESWITCH_FREE_HISTOGRAM " in line:
        values = {k: int(v) for k,v in re.findall(r"(\w+)=(\d+)", line)}
        if values.get("id") in free_by_id:
            free_by_id[values.pop("id")]["histogram"] = values
    elif "MINESWITCH_ALLOC_SUMMARY " in line:
        values = {k: int(v) for k,v in re.findall(r"(\w+)=(\d+)", line)}
        samples.append({"elapsed_s": elapsed, **values, "largest": [], "owners": [], "native": native})
    elif samples and any(marker in line for marker in ("MINESWITCH_VM_BACKING ", "MINESWITCH_MALLOC_MEMORY ", "MINESWITCH_NXVK_MEMORY ")):
        marker = next(name for name in ("VM_BACKING", "MALLOC_MEMORY", "NXVK_MEMORY") if "MINESWITCH_" + name + " " in line)
        samples[-1][marker.lower()] = {k: int(v) for k,v in re.findall(r"(\w+)=(\d+)", line)}
    elif "MINESWITCH_ALLOC_LIVE " in line and samples:
        values = dict(re.findall(r"(\w+)=(0x[0-9a-f]+|\d+)", line))
        caller = int(values["caller"], 16)
        samples[-1]["largest"].append({"bytes": int(values["bytes"]), "pointer": values["pointer"], **lookup[caller]})
    elif "MINESWITCH_ALLOC_OWNER " in line and samples:
        values = dict(re.findall(r"(\w+)=(0x[0-9a-f]+|\d+)", line))
        caller = int(values["caller"], 16)
        samples[-1]["owners"].append({"bytes": int(values["bytes"]), "count": int(values["count"]), **lookup[caller]})
    elif "MINESWITCH_ALLOC kind=" in line:
        values = dict(re.findall(r"(\w+)=(0x[0-9a-f]+|[a-z_]+|\d+|\(nil\))", line))
        caller = int(values["caller"], 16)
        events.append({"elapsed_s": elapsed, **values, **lookup[caller]})
    elif "MINESWITCH_GPU_BO " in line:
        values = dict(re.findall(r"(\w+)=(0x[0-9a-f]+|-?\d+)", line))
        caller = int(values["caller"], 16)
        memory_kind = values.pop("kind", None)
        events.append({"elapsed_s": elapsed, "kind": "gpu_bo", "memory_kind": memory_kind, **values, **lookup[caller]})
    elif "MINESWITCH_REGION " in line:
        regions.append(dict(re.findall(r"(\w+)=(0x[0-9a-f]+|\d+)", line)))
result = {"log": str(args.log), "elf": str(elf), "image_base": hex(base),
          "scope": "Public allocation calls after trace start, blocks at least256KiB;8 largest live blocks per sample;inflight allocations can be between records.",
          "regions": regions, "samples": samples, "events": events, "free_blocks": free_blocks,
          "reclaims": reclaims, "mimalloc_samples": mimalloc_samples,
          "gpu_alias_samples": gpu_alias_samples,
          "large_page_samples": large_page_samples,
          "accounting_note": "GPU alias backing is included in NVK physical backing and mimalloc pool ownership; do not add it again. Public malloc traces exclude alias backing allocated directly from the pool."}
if args.output:
    args.output.write_text(json.dumps(result, indent=2) + "\n")
for sample in samples[-2:]:
    print(f"t={sample['elapsed_s']}s tracked={sample['bytes']/2**20:.1f}MiB count={sample['count']} dropped={sample['dropped']}")
    for item in sample["largest"]:
        print(f"  {item['bytes']/2**20:8.2f}MiB {item['function']} {item['source']}")
    for item in sample["owners"]:
        print(f"  owner {item['bytes']/2**20:8.2f}MiB in {item['count']} blocks: {item['function']}")
    for name in ("nxvk_memory", "vm_backing", "malloc_memory"):
        if name in sample:
            print(f"  {name}: {sample[name]}")
for event in events:
    if event.get("success") == "0":
        print(f"FAILED t={event['elapsed_s']} bytes={event['bytes']} {event['function']} {event['source']}")
for row in free_blocks:
    if row.get("scope") == "failure":
        print(f"FREE_AT_FAILURE t={row['elapsed_s']} capture={row['capture']} request={row['requested']} "
              f"free={row['free']} largest_request={row['largest_request']} fitting={row['fitting']} "
              f"invalid={row['invalid']} truncated={row['truncated']}")
for row in reclaims:
    print(f"RECLAIM t={row['elapsed_s']} requested={row['requested']} released={row['released']} "
          f"retry_success={row['retry_success']}")
for row in mimalloc_samples[-2:]:
    print(f"MIMALLOC t={row['elapsed_s']} backing={row['backing']/2**20:.2f}MiB "
          f"pool_free={row['pool_free']/2**20:.2f}MiB largest_extent={row['largest_pool_extent']/2**20:.2f}MiB "
          f"primitive_failures={row['failures']}")
for row in gpu_alias_samples[-2:]:
    print(f"GPU_ALIAS t={row['elapsed_s']} backing={row['backing']/2**20:.2f}MiB "
          f"active={row['active']} allocations={row['allocations']} failures={row['failures']} "
          f"last_rc=0x{row['last_rc']:08x}")
