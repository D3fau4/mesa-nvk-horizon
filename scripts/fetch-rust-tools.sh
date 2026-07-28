#!/usr/bin/env bash
# Fetches, on the host, everything the toolchain image needs in order
# to carry bindgen, cbindgen and libclang: the two Debian packages, the
# two crates, and their vendored dependencies.
#
#   scripts/fetch-rust-tools.sh [--force]
#
# This script downloads and verifies. It compiles nothing — that
# happens in scripts/build-toolchain-image.sh, inside the image, where
# the results have to run.
#
# WHY THE SPLIT. `cargo install bindgen-cli cbindgen` is the documented
# way to get these two, and neither half of it works here:
#
#   - Run inside the toolchain image, it needs the network, and
#     containers here have none (CLAUDE.md). That includes the
#     container `docker build` runs its RUN steps in.
#   - Run on the host, it produces an Ubuntu 24.04 binary (glibc 2.39)
#     and the image is Debian 12 (glibc 2.36), so it cannot execute
#     where the build runs.
#
# So: fetch on the host, where the network is; build in the image,
# where the result has to run. Same rule scripts/fetch-mesa.sh and
# scripts/fetch-rust-crates.sh already follow.
#
# libclang cannot be built here at all, so it comes from Debian
# bookworm's own archive — the distribution the image is — verified by
# sha256 against toolchain/versions.env.
#
# Idempotent: an already-downloaded file with a matching hash is left
# alone, and the vendor step is skipped if it has already run for these
# versions. --force redoes everything.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

DL=build/toolchain/rust-tools-dl
SRC=build/toolchain/rust-tools-src
VENDOR=build/toolchain/rust-tools-vendor
VENDOR_CFG=build/toolchain/rust-tools-vendor-config.toml
STAMP="$VENDOR/.stamp"

identity() {
    echo "bindgen-cli=$BINDGEN_CLI_VERSION cbindgen=$CBINDGEN_VERSION"
    echo "libclang=$DEBIAN_LIBCLANG_DEB libllvm=$DEBIAN_LIBLLVM_DEB"
}

if [ "$FORCE" -eq 0 ] && [ -f "$STAMP" ] &&
   [ "$(identity)" = "$(cat "$STAMP")" ]; then
    echo "fetch-rust-tools: sources for bindgen $BINDGEN_CLI_VERSION," \
         "cbindgen $CBINDGEN_VERSION and libclang are present"
    exit 0
fi

mkdir -p "$DL" "$SRC"

# Downloads $2 from $1 and checks it against $3. A mismatched file is
# removed rather than kept, so a truncated download cannot be mistaken
# for a deliberate pin change on the next run.
fetch_verified() { # url-base file sha256
    _out="$DL/$2"
    if [ -f "$_out" ] &&
       [ "$(sha256sum "$_out" | cut -d' ' -f1)" = "$3" ]; then
        return 0
    fi
    echo "fetch-rust-tools: downloading $2"
    curl -fsSL --retry 3 -o "$_out.part" "$1/$2"
    _got=$(sha256sum "$_out.part" | cut -d' ' -f1)
    if [ "$_got" != "$3" ]; then
        rm -f "$_out.part"
        echo "error: $2 hashes to $_got, but toolchain/versions.env" >&2
        echo "       says $3. Refusing to use it." >&2
        exit 1
    fi
    mv "$_out.part" "$_out"
}

# --- 1. libclang and the libLLVM it needs, from Debian bookworm -------
#
# Two packages, not one: libclang1-16 declares Depends on libllvm16,
# and readelf confirms NEEDED libLLVM-16.so.1. Everything else those
# two depend on (libedit, libz3, libxml2, libzstd, libtinfo, ...) is
# already in the base image — measured by running bindgen there over a
# real header, not by reading the dependency list.
fetch_verified "$DEBIAN_LIBCLANG_URL" "$DEBIAN_LIBCLANG_DEB" \
               "$DEBIAN_LIBCLANG_SHA256"
fetch_verified "$DEBIAN_LIBCLANG_URL" "$DEBIAN_LIBLLVM_DEB" \
               "$DEBIAN_LIBLLVM_SHA256"

# --- 2. the two tools' sources ----------------------------------------
crate_url=https://static.crates.io/crates
fetch_verified "$crate_url/bindgen-cli" \
               "bindgen-cli-$BINDGEN_CLI_VERSION.crate" \
               "$BINDGEN_CLI_SHA256"
fetch_verified "$crate_url/cbindgen" \
               "cbindgen-$CBINDGEN_VERSION.crate" \
               "$CBINDGEN_SHA256"

for pkg in "bindgen-cli-$BINDGEN_CLI_VERSION" "cbindgen-$CBINDGEN_VERSION"; do
    if [ ! -d "$SRC/$pkg" ] || [ "$FORCE" -eq 1 ]; then
        rm -rf "$SRC/$pkg"
        tar -xzf "$DL/$pkg.crate" -C "$SRC"
    fi
done

# --- 3. their dependencies, vendored ----------------------------------
#
# cargo vendor is the step that needs the network, so it runs here. Its
# output is plain sources plus per-file checksums, so it does not
# matter that the host's cargo is a different version from the image's:
# nothing compiled is produced.
#
# -s adds the second package's manifest to the same vendor directory.
# Without it, a second `cargo vendor` would delete the first's crates.
echo "fetch-rust-tools: vendoring the tools' dependencies"
rm -rf "$VENDOR"
cargo vendor \
    --manifest-path "$SRC/bindgen-cli-$BINDGEN_CLI_VERSION/Cargo.toml" \
    -s "$SRC/cbindgen-$CBINDGEN_VERSION/Cargo.toml" \
    "$VENDOR" > "$VENDOR_CFG"

identity > "$STAMP"
echo "fetch-rust-tools: $DL, $SRC and $VENDOR are ready;" \
     "scripts/build-toolchain-image.sh builds them"
