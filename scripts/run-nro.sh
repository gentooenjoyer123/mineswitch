#!/usr/bin/env bash
set -euo pipefail

task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
toolchain_root="${DEVKITPRO:-${task_root}/.toolchain/opt/devkitpro}"
switch_address="${1:?Usage: bash scripts/run-nro.sh SWITCH_IP PATH_TO_NRO}"
nro_path="${2:?Usage: bash scripts/run-nro.sh SWITCH_IP PATH_TO_NRO [APP_ARGS...]}"
shift 2
app_arguments=("$@")
# NXLink has a small fixed argument buffer. Keep long classpaths/settings in
# the application's options file and preserve argument boundaries here.
argument_bytes=0
for argument in "${app_arguments[@]}"; do
    argument_bytes=$((argument_bytes + ${#argument} + 1))
done
if (( argument_bytes > 768 )); then
    echo 'Application arguments exceed the NXLink budget; use a configuration file.' >&2
    exit 1
fi
nxlink_path="${toolchain_root}/tools/bin/nxlink"

if [[ ! -x "${nxlink_path}" ]]; then
    if ! nxlink_path="$(command -v nxlink)"; then
        echo 'nxlink not found; install the toolchain or add nxlink to PATH.' >&2
        exit 1
    fi
fi
if [[ ! -f "${nro_path}" || "${nro_path}" != *.nro ]]; then
    echo "NRO file not found: ${nro_path}" >&2
    exit 1
fi

mkdir -p "${task_root}/logs"
# Keep a single complete NXLink transaction active; never open a separate TCP
# connection to this loader as a readiness probe.
exec 9>"${task_root}/logs/.nxlink.lock"
if ! flock -n 9; then
    echo 'Another NXLink upload is running. Wait for it to finish.' >&2
    exit 1
fi
app_name="$(basename -- "${nro_path}" .nro)"
run_prefix="${task_root}/logs/${app_name}-$(date -u +%Y%m%dT%H%M%SZ)"
sha256sum "${nro_path}" > "${run_prefix}.sha256"
elf_path="${nro_path%.nro}.elf"
if [[ -f "${elf_path}" ]]; then
    cp -- "${elf_path}" "${run_prefix}.elf"
fi

echo "Saving NXLink output to ${run_prefix}.log"
printf 'Starting NXLink upload to %s: %s\n' "${switch_address}" "${nro_path}" \
    | tee "${run_prefix}.log"
if command -v stdbuf >/dev/null 2>&1; then
    stdbuf -oL -eL "${nxlink_path}" -a "${switch_address}" -s -- "${nro_path}" "${app_arguments[@]}" \
        2>&1 | tee -a "${run_prefix}.log"
else
    "${nxlink_path}" -a "${switch_address}" -s -- "${nro_path}" "${app_arguments[@]}" \
        2>&1 | tee -a "${run_prefix}.log"
fi
