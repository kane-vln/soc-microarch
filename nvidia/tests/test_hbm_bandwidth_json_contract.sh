#!/usr/bin/env bash
set -euo pipefail

bin="${1:?usage: test_hbm_bandwidth_json_contract.sh /path/to/nvidia_gb200_probe}"
if [[ ! -x "${bin}" ]]; then
  echo "expected executable at ${bin}" >&2
  exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT

out="${tmpdir}/hbm.json"
"${bin}" --probe hbm-bandwidth --gpu 0 --bytes 67108864 --output "${out}" >/tmp/gb200_hbm_probe.out 2>&1

for needle in \
  '"hbm-bandwidth"' \
  '"read"' \
  '"write"' \
  '"copy"' \
  '"gb_per_second"' \
  '"bytes"' \
  '"validation"'; do
  grep -F -- "${needle}" "${out}" >/dev/null
done

if command -v jq >/dev/null 2>&1; then
  jq -e '.probes["hbm-bandwidth"].parameters.bytes == 67108864' "${out}" >/dev/null
  jq -e '.probes["hbm-bandwidth"].summary.read.gb_per_second > 0' "${out}" >/dev/null
  jq -e '.probes["hbm-bandwidth"].summary.write.gb_per_second > 0' "${out}" >/dev/null
  jq -e '.probes["hbm-bandwidth"].summary.copy.gb_per_second > 0' "${out}" >/dev/null
fi
