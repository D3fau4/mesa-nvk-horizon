#!/usr/bin/env bash
# Reports what the current environment's Switch toolchain provides.
#
#   scripts/print-toolchain-versions.sh
#
# Read-only, and deliberately nothing more. The Switch toolchain — libnx
# above all — belongs to the environment, not to this project
# (toolchain/versions.env, ENVIRONMENT section): it is not pinned here,
# not compared against a recorded value here, and not updated from here.
# Updating it is `dkp-pacman -Syu` in your devkitPro install, or running
# a newer container image.
#
# So this script answers one question — "what am I building against
# right now?" — for a human reading it, and for
# scripts/package-horizon.sh, which embeds the output in the artefact
# manifest so a hardware result stays attributable to a specific build.
#
# Starts no daemon and leaves no container behind (docker run --rm).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

probe() {
    cat <<'PROBE'
set -eu
PATH="${DEVKITPRO}/devkitA64/bin:/opt/cargo/bin:${PATH}"

# Test the captured value, not the pipeline's exit status: with
# dkp-pacman absent the pipeline still succeeds (awk does), so a
# trailing `|| echo unknown` never fires and the field silently comes
# out blank. That matters on a local devkitPro install without
# dkp-pacman, where a blank reads as "nothing installed" rather than
# "not answerable".
pkg() {
    v=$(dkp-pacman -Q "$1" 2>/dev/null | awk '{print $2}')
    if [ -n "$v" ]; then echo "$v"; else echo unknown; fi
}

echo "devkitA64          $(pkg devkitA64)"
echo "  gcc              $(pkg devkita64-gcc)"
echo "  binutils         $(pkg devkita64-binutils)"
echo "  newlib           $(pkg devkita64-newlib)"
echo "  rules            $(pkg devkita64-rules)"
echo "libnx (package)    $(pkg libnx)"
echo "deko3d             $(pkg deko3d)"
echo "switch-tools       $(pkg switch-tools)"
echo "general-tools      $(pkg general-tools)"
echo "dkp-meson-scripts  $(pkg dkp-meson-scripts)"
echo "target triple      $(aarch64-none-elf-gcc -dumpmachine 2>/dev/null || echo unknown)"
echo "rustc              $(rustc --version 2>/dev/null || echo absent)"

# Why the libnx package version above is not an identifier: the image
# builds switchbrew/libnx from git HEAD on top of the package, so most
# of the installed tree is newer than the version number claims. This
# line measures how much (known risk R15).
altered=$(dkp-pacman -Qkk libnx 2>&1 | sed -n 's/.*, \([0-9]*\) altered files/\1/p')
total=$(dkp-pacman -Qkk libnx 2>&1 | sed -n 's/^libnx: \([0-9]*\) total files.*/\1/p')
echo "libnx (installed)  git build over the package: ${altered:-unknown} of ${total:-unknown} files differ"
PROBE
}

echo "toolchain          $HORIZON_TOOLCHAIN_DESC"
# Not "digest": horizon_image_digest returns a full repo@sha256:...
# reference when the image has one, "local-image-id:sha256:..." when it
# was built here and never pushed, or "local"/"unknown". Labelling all
# of those "digest" mislabels the field in the one place whose purpose
# is telling a reader exactly what produced an artefact.
echo "image              $(horizon_image_digest)"

if [ "$HORIZON_IN_CONTAINER" -eq 0 ]; then
    probe | sh
else
    probe | docker run --rm -i \
        -e DEVKITPRO="$HORIZON_DEVKITPRO" "$HORIZON_IMAGE" sh 2>/dev/null
fi
