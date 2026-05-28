#!/usr/bin/env bash
set -euo pipefail

bin="${1:?usage: test_cli_contract.sh /path/to/nvidia_gb200_probe}"

if [[ ! -x "${bin}" ]]; then
  echo "expected executable at ${bin}" >&2
  exit 1
fi

help_output="$("${bin}" --help)"

for needle in \
  "--probe env|ptx-latency|mem-chase|hbm-bandwidth|fabric-smoke|all" \
  "--gpu <id>" \
  "--gpus <csv>" \
  "--mode smoke|custom" \
  "--iterations <n>" \
  "--mem-sizes <csv>" \
  "--bytes <n>" \
  "--output <path>"; do
  grep -F -- "${needle}" <<<"${help_output}" >/dev/null
done

if "${bin}" --probe does-not-exist --output /tmp/gb200_invalid_probe.json >/tmp/gb200_invalid_probe.out 2>&1; then
  echo "invalid probe unexpectedly succeeded" >&2
  exit 1
fi

grep -F -- "unknown probe" /tmp/gb200_invalid_probe.out >/dev/null
