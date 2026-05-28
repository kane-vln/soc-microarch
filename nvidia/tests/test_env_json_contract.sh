#!/usr/bin/env bash
set -euo pipefail

bin="${1:?usage: test_env_json_contract.sh /path/to/nvidia_gb200_probe}"
if [[ ! -x "${bin}" ]]; then
  echo "expected executable at ${bin}" >&2
  exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT

out="${tmpdir}/env.json"
"${bin}" --probe env --output "${out}" >/tmp/gb200_env_probe.out 2>&1

for needle in \
  '"run"' \
  '"devices"' \
  '"probes"' \
  '"warnings"' \
  '"env"' \
  '"p2p_access_matrix"' \
  '"compute_capability_major"' \
  '"validation"'; do
  grep -F -- "${needle}" "${out}" >/dev/null
done

if command -v jq >/dev/null 2>&1; then
  jq -e '.devices | length >= 1' "${out}" >/dev/null
  jq -e '.probes.env.parameters.probe == "env"' "${out}" >/dev/null
fi
