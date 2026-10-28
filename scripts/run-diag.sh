#!/usr/bin/env bash
set -euo pipefail

task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "${task_root}/scripts/run-nro.sh" \
    "${1:?Usage: bash scripts/run-diag.sh SWITCH_IP}" \
    "${task_root}/diagnostic/mineswitch-diag.nro"
