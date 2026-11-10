#!/usr/bin/env bash
set -euo pipefail
# Usage: bash reproduce.sh /absolute/path/to/telemetry_sim [new_output_directory]
experiment_root="$(cd -- "$(dirname -- "$0")" && pwd)"
program_path="${1:?Specify the telemetry_sim executable}"
output_root="${2:-$experiment_root/reproduced_results}"
for scenario in A_reference B_crash C_heartbeat_gap D_heterogeneous; do
  "$program_path" compare --scenario "$experiment_root/inputs/$scenario/scenario.json" --out "$output_root/$scenario"
done
