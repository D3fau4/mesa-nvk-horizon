#!/usr/bin/env bash
# Builds the derived toolchain image: the environment's nx-dev plus
# libclang, bindgen, cbindgen and a Rust sysroot for the Switch target.
#
#   scripts/build-toolchain-image.sh [--force]
#
# See toolchain/Dockerfile for what each addition is and why it cannot
# simply be installed. This script's job is the two things a Dockerfile
# cannot do here: fetch the material (containers have no network, and
# that includes the one `docker build` runs its RUN steps in), and
# assemble a context out of it.
#
# Only meaningful in container mode. With $DEVKITPRO set there is no
# image at all; scripts/build-rust-sysroot.sh covers the sysroot then,
# and bindgen and cbindgen come from the developer's own PATH.
#
# Idempotent: an image already built from the current pins and the
# current base image is left alone. --force rebuilds it.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ "$HORIZON_IN_CONTAINER" -eq 0 ]; then
    echo "build-toolchain-image: \$DEVKITPRO is set, so the toolchain is"
    echo "                       this machine and there is no image to"
    echo "                       build. Nothing to do."
    exit 0
fi

CTX=build/toolchain/image-context

# What the image was built from. The base image is a floating tag by
# policy, so its digest is part of the identity: a `docker pull` that
# moves the tag has to rebuild this layer, or the sysroot inside it
# would be compiled by one rustc and used by another — which fails as
# "found crate `core` compiled by an incompatible version", far from
# the cause.
image_identity() {
    echo "base=$HORIZON_BASE_IMAGE"
    echo "base-id=$(docker image inspect --format '{{.Id}}' \
                        "$HORIZON_BASE_IMAGE" 2>/dev/null || echo unknown)"
    echo "bindgen-cli=$BINDGEN_CLI_VERSION"
    echo "cbindgen=$CBINDGEN_VERSION"
    echo "libclang=$DEBIAN_LIBCLANG_DEB"
    echo "libllvm=$DEBIAN_LIBLLVM_DEB"
    # THE RESOLVED CLOSURE, WHICH IS THE .deb FILES THEMSELVES.
    #
    # This read build/toolchain/clc-deps/closure.txt, and nothing has
    # ever written that file — scripts/fetch-clc-deps.sh produces
    # `debs/` and an `installed.txt` that lists what the base image
    # already had. So `sha256sum` failed silently on every run since
    # this function was written, the field was always empty, and a
    # change in the resolved closure could not have rebuilt the image:
    # exactly the drift this line exists to catch, undetected by the
    # line meant to catch it.
    #
    # The names carry the versions, so the sorted list of them is the
    # closure's identity; hashing 42 .deb payloads would say the same
    # thing far more slowly.
    echo "clc-closure=$(ls build/toolchain/clc-deps/debs 2>/dev/null |
                            sort | sha256sum | cut -d' ' -f1)"
    echo "spirv-tools=$SPIRV_TOOLS_COMMIT"
    echo "target=$RUST_TARGET"
    echo "dockerfile=$(sha256sum toolchain/Dockerfile | cut -d' ' -f1)"
}

built_identity() {
    docker image inspect --format \
        '{{index .Config.Labels "org.mesa-nvk-horizon.identity"}}' \
        "$HORIZON_DERIVED_IMAGE" 2>/dev/null || true
}

# The base image has to be local before its ID can be part of the
# identity above. Pulling is the one network operation here, and it
# happens on the host.
if ! docker image inspect "$HORIZON_BASE_IMAGE" >/dev/null 2>&1; then
    echo "build-toolchain-image: pulling $HORIZON_BASE_IMAGE"
    docker pull "$HORIZON_BASE_IMAGE"
fi

want=$(image_identity | sha256sum | cut -d' ' -f1)
if [ "$FORCE" -eq 0 ] && [ "$(built_identity)" = "$want" ]; then
    echo "build-toolchain-image: $HORIZON_DERIVED_IMAGE is current"
    exit 0
fi

# --- 1. the material, fetched on the host -----------------------------
scripts/fetch-rust-tools.sh
scripts/fetch-rust-crates.sh
scripts/fetch-clc-deps.sh

# The closure file did not exist the first time image_identity ran, so
# the label has to be recomputed now that it does. Without this the
# image is labelled with an identity no later run can reproduce, and it
# rebuilds itself every time.
want=$(image_identity | sha256sum | cut -d' ' -f1)

# AND ASK AGAIN, because the first answer was given without the closure.
#
# On a tree where the resolved .deb closure is not there yet — every
# fresh clone, and therefore every CI checkout — the check at the top
# computes an identity that no image can match, so it always falls
# through to here. Before this, that meant a full rebuild even when the
# image on this machine was already exactly the wanted one. Nine minutes,
# for an answer that was available as soon as the fetches finished.
#
# It is what makes pulling the image from a registry worth anything: the
# pull lands under $HORIZON_DERIVED_IMAGE, and this is the line that
# recognises it rather than building over it.
if [ "$FORCE" -eq 0 ] && [ "$(built_identity)" = "$want" ]; then
    echo "build-toolchain-image: $HORIZON_DERIVED_IMAGE is current" \
         "(identity confirmed after fetching)"
    exit 0
fi

# --- 2. the build context ---------------------------------------------
#
# Assembled rather than pointed at build/toolchain directly: that
# directory also holds Meson, a 250 MB cargo target directory and the
# Mesa tarball cache, none of which belong in an image context.
echo "build-toolchain-image: assembling the context in $CTX"
rm -rf "$CTX"
mkdir -p "$CTX/debs" "$CTX/sysroot-driver/src"

cp "build/toolchain/rust-tools-dl/$DEBIAN_LIBCLANG_DEB" \
   "build/toolchain/rust-tools-dl/$DEBIAN_LIBLLVM_DEB" "$CTX/debs/"
cp -a build/toolchain/rust-tools-src "$CTX/rust-tools-src"
cp -a build/toolchain/rust-tools-vendor "$CTX/rust-tools-vendor"
cp -a build/toolchain/rust-vendor "$CTX/rust-vendor"
cp -a build/toolchain/clc-deps/debs "$CTX/clc-debs"
mkdir -p "$CTX/spirv-src"
cp -a build/toolchain/clc-deps/src/SPIRV-Tools "$CTX/spirv-src/SPIRV-Tools"
cp -a build/toolchain/clc-deps/src/SPIRV-Headers "$CTX/spirv-src/SPIRV-Headers"

# The two cargo source-replacement configs, rewritten to the paths the
# vendor trees will have *inside* the image. The generated ones name
# this checkout, which does not exist there.
cat > "$CTX/cargo-config-tools.toml" <<'EOF'
# GENERATED by scripts/build-toolchain-image.sh — do not edit.
[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "/tmp/horizon-ctx/rust-tools-vendor"
EOF
cat > "$CTX/cargo-config-std.toml" <<'EOF'
# GENERATED by scripts/build-toolchain-image.sh — do not edit.
[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "/tmp/horizon-ctx/rust-vendor"
EOF

# The package -Zbuild-std is invoked through. It exists only to give
# cargo something to build; core and alloc come along as its
# dependencies and are what gets installed.
cat > "$CTX/sysroot-driver/Cargo.toml" <<'EOF'
# GENERATED by scripts/build-toolchain-image.sh — do not edit.
[package]
name = "horizon-sysroot-driver"
version = "0.0.0"
edition = "2021"

[lib]
path = "src/lib.rs"
crate-type = ["rlib"]

# The target spec already says panic = abort. Stating it here as well
# keeps the driver's own profile from disagreeing with the target and
# asking for an unwinder that does not exist.
[profile.dev]
panic = "abort"
[profile.release]
panic = "abort"
EOF
cat > "$CTX/sysroot-driver/src/lib.rs" <<'EOF'
// GENERATED by scripts/build-toolchain-image.sh — do not edit.
#![no_std]
extern crate alloc;
EOF

# The two probes toolchain/Dockerfile § 4b runs. They live in the
# context because a RUN step can only see what was COPYed in — which is
# the whole point: no bind mount, so this works from inside a job
# container as well as from a developer's shell.
mkdir -p "$CTX/probe"
printf 'struct probe_s { int a; unsigned long b; };\n' > "$CTX/probe/probe.h"
cat > "$CTX/probe/s.rs" <<'EOF'
// GENERATED by scripts/build-toolchain-image.sh — do not edit.
#![no_std]
extern crate alloc;
use core::alloc::{GlobalAlloc, Layout};
extern "C" {
    fn memalign(align: usize, size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
    fn abort() -> !;
}
struct A;
unsafe impl GlobalAlloc for A {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        unsafe { memalign(l.align(), l.size()) }
    }
    unsafe fn dealloc(&self, p: *mut u8, _l: Layout) { unsafe { free(p) } }
}
#[global_allocator]
static ALLOC: A = A;
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { unsafe { abort() } }
#[no_mangle]
pub extern "C" fn horizon_sysroot_probe(n: u32) -> u32 {
    let v: alloc::vec::Vec<u32> = (0..n).collect();
    v.iter().sum()
}
EOF

# --- 3. build ----------------------------------------------------------
#
# --network=none is not an optimisation. dockerd here runs with
# --bridge=none, and without this flag every RUN step fails with
# "network bridge not found" before it executes anything. Measured.
#
# It is built under a staging tag and only named $HORIZON_DERIVED_IMAGE
# once the checks below pass. Otherwise a build that succeeds and a
# toolchain that works are the same claim — and they are not: the first
# run of this script produced an image whose sysroot check failed, and
# because the identity label was already baked in under the final name,
# the next run reported it "current" and skipped the check entirely. A
# gate that a failure can switch off is not a gate.
STAGING="$HORIZON_NX_DERIVED_REPO:staging"
echo "build-toolchain-image: building $HORIZON_DERIVED_IMAGE"
docker build --network=none \
    -f toolchain/Dockerfile \
    -t "$STAGING" \
    --build-arg "BASE_IMAGE=$HORIZON_BASE_IMAGE" \
    --build-arg "RUST_TARGET=$RUST_TARGET" \
    --build-arg "BINDGEN_CLI_VERSION=$BINDGEN_CLI_VERSION" \
    --build-arg "CBINDGEN_VERSION=$CBINDGEN_VERSION" \
    --build-arg "IMAGE_IDENTITY=$want" \
    --label "org.mesa-nvk-horizon.identity=$want" \
    "$CTX"

rm -rf "$CTX"

# --- 4. prove it works before claiming success ------------------------
#
# A built image is not a working toolchain. `bindgen --version` in
# particular proves nothing: clang-sys loads libclang lazily, so the
# version string prints happily on an image with no libclang at all —
# which is exactly how the first attempt here passed while being
# broken. Generating bindings from a real header is the check that
# fails when it should.
# The probes now run inside `docker build` (toolchain/Dockerfile § 4b),
# because `docker run -v "$PWD":"$PWD"` cannot work when this script is
# itself inside a container: $PWD is the job container's path, the
# daemon resolves the bind against the host, finds nothing, and mounts
# an empty directory. bindgen then reported `No such file or directory`
# about a header that was right there — Forgejo task 1274.
#
# A probe that is a build step also cannot leave a broken image behind
# to be un-tagged afterwards: the build simply fails.
echo "build-toolchain-image: probes ran inside the build; image is good"

# Only now does the image get the name everything else looks for.
docker tag "$STAGING" "$HORIZON_DERIVED_IMAGE"
docker rmi "$STAGING" >/dev/null

echo "build-toolchain-image: $HORIZON_DERIVED_IMAGE ready" \
     "($(docker image inspect --format '{{.Size}}' "$HORIZON_DERIVED_IMAGE" |
         awk '{printf "%.0f MB", $1/1048576}'))"
