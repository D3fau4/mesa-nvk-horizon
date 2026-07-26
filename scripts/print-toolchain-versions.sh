#!/usr/bin/env bash
# Re-derives every toolchain version from the live toolchain and, with
# --check, diffs them against toolchain/versions.env.
#
# A pin nobody can re-derive is a comment, not a pin. This script is what
# makes toolchain/versions.env checkable: run it after changing the
# image, or when a build behaves differently from what the file claims.
#
#   scripts/print-toolchain-versions.sh          # print KEY=value
#   scripts/print-toolchain-versions.sh --check  # compare, exit 1 on drift
#
# Read-only and idempotent: it starts no daemon, writes nothing, and
# leaves no container behind (docker run --rm).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=../toolchain/versions.env
. toolchain/versions.env

mode="print"
[ "${1:-}" = "--check" ] && mode="check"

# The probe runs inside the pinned image when there is no local
# devkitA64, and directly otherwise. Either way it emits KEY=value on
# stdout using the same field names as toolchain/versions.env.
probe() {
    cat <<'PROBE'
set -eu
PATH="${DEVKITPRO}/devkitA64/bin:/opt/cargo/bin:${PATH}"

pkg() { dkp-pacman -Q "$1" 2>/dev/null | awk '{print $2}'; }

echo "DEVKITA64_VERSION=$(pkg devkitA64)"
echo "DEVKITA64_GCC_VERSION=$(pkg devkita64-gcc)"
echo "DEVKITA64_BINUTILS_VERSION=$(pkg devkita64-binutils)"
echo "DEVKITA64_NEWLIB_VERSION=$(pkg devkita64-newlib)"
echo "DEVKITA64_RULES_VERSION=$(pkg devkita64-rules)"
echo "SWITCH_TOOLS_VERSION=$(pkg switch-tools)"
echo "GENERAL_TOOLS_VERSION=$(pkg general-tools)"
echo "DKP_MESON_SCRIPTS_VERSION=$(pkg dkp-meson-scripts)"
echo "DKP_TOOLCHAIN_VARS_VERSION=$(pkg dkp-toolchain-vars)"
echo "LIBNX_PACKAGE_VERSION=$(pkg libnx)"
echo "DEKO3D_VERSION=$(pkg deko3d)"
echo "DEVKITA64_TARGET_TRIPLE=$(aarch64-none-elf-gcc -dumpmachine)"
echo "NINJA_VERSION=$(ninja --version 2>/dev/null || echo none)"
echo "PYTHON_VERSION=$(python3 -c 'import platform;print(platform.python_version())')"
echo "RUST_VERSION=$(rustc --version 2>/dev/null | awk '{print $2}' || echo none)"
echo "RUST_COMMIT_HASH=$(rustc --version 2>/dev/null | tr -d '()' | awk '{print $3}' || echo none)"
echo "RUST_COMMIT_DATE=$(rustc --version 2>/dev/null | tr -d '()' | awk '{print $4}' || echo none)"
echo "RUST_CARGO_VERSION=$(cargo --version 2>/dev/null | awk '{print $2}' || echo none)"

# R15 evidence: how much of the libnx package the image's git build
# overwrote. A package version that describes 21 of 226 files is not a
# pin, and this is the number that says so.
altered=$(dkp-pacman -Qkk libnx 2>&1 | sed -n 's/.*, \([0-9]*\) altered files/\1/p')
total=$(dkp-pacman -Qkk libnx 2>&1 | sed -n 's/^libnx: \([0-9]*\) total files.*/\1/p')
echo "LIBNX_PKG_FILES_ALTERED=${altered:-unknown}/${total:-unknown}"
PROBE
}

if [ -n "${DEVKITPRO:-}" ]; then
    actual=$(probe | sh)
    source_desc="local devkitA64 (\$DEVKITPRO)"
else
    command -v docker >/dev/null || {
        echo "error: no \$DEVKITPRO and no docker" >&2; exit 1; }
    image="${HORIZON_NX_IMAGE_REPO}@${HORIZON_NX_IMAGE_DIGEST}"
    actual=$(probe | docker run --rm -i \
        -e DEVKITPRO="${HORIZON_NX_IMAGE_DEVKITPRO}" "$image" sh 2>/dev/null)
    source_desc="$image"
fi

if [ "$mode" = "print" ]; then
    echo "# derived from: $source_desc"
    echo "$actual"
    exit 0
fi

echo "checking toolchain/versions.env against $source_desc"
drift=0
while IFS= read -r line; do
    key=${line%%=*}
    got=${line#*=}
    # LIBNX_PKG_FILES_ALTERED is evidence printed for the reader, not a
    # pinned field; versions.env records it in a comment.
    [ "$key" = "LIBNX_PKG_FILES_ALTERED" ] && {
        echo "  note: libnx package files altered by the git build: $got"
        continue
    }
    eval "want=\${$key:-<unset>}"
    if [ "$want" != "$got" ]; then
        echo "  DRIFT $key: versions.env=$want actual=$got"
        drift=1
    fi
done <<EOF
$actual
EOF

if [ "$drift" -eq 0 ]; then
    echo "print-toolchain-versions: OK (no drift)"
else
    echo "print-toolchain-versions: DRIFT — re-pin toolchain/versions.env" >&2
fi
exit "$drift"
