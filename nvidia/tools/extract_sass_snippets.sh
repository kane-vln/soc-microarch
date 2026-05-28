#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: extract_sass_snippets.sh input.sass output.md" >&2
  exit 2
fi

input="$1"
output="$2"

if [[ ! -f "${input}" ]]; then
  echo "missing SASS input: ${input}" >&2
  exit 1
fi

mkdir -p "$(dirname "${output}")"

{
  echo "# SASS Snippets"
  echo
  echo "Source: \`${input}\`"
  echo
  for pattern in IADD3 FFMA LDG STG; do
    echo "## ${pattern}"
    echo
    echo '```sass'
    grep -n -m 12 -E "\\b${pattern}\\b" "${input}" || true
    echo '```'
    echo
  done
} >"${output}"
