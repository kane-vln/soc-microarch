#!/usr/bin/env bash
set -euo pipefail

helper="${1:?usage: test_sass_snippet_helper.sh /path/to/extract_sass_snippets.sh}"
if [[ ! -x "${helper}" ]]; then
  echo "expected executable helper at ${helper}" >&2
  exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT

sass="${tmpdir}/sample.sass"
out="${tmpdir}/snippets.md"
cat >"${sass}" <<'EOF'
        /*0010*/                   IADD3 R1, R1, 0x1, RZ ;
        /*0020*/                   FFMA R2, R2, R3, R4 ;
        /*0030*/                   LDG.E R5, desc[UR6][R8.64] ;
EOF

"${helper}" "${sass}" "${out}"

grep -F -- "IADD3" "${out}" >/dev/null
grep -F -- "FFMA" "${out}" >/dev/null
grep -F -- "LDG" "${out}" >/dev/null
