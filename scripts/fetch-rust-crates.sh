#!/usr/bin/env bash
# Vendors the crates.io dependencies of Rust's own standard-library
# workspace, so `cargo -Zbuild-std` can build core/alloc for
# aarch64-nintendo-switch-freestanding inside the toolchain container.
#
#   scripts/fetch-rust-crates.sh [--force]
#
# WHY THIS EXISTS, measured rather than assumed:
#
#   $ cargo build --offline -Z build-std=core,alloc \
#         --target aarch64-nintendo-switch-freestanding
#   error: no matching package named `cfg-if` found
#   location searched: crates.io index
#   required by package `std v0.0.0 (.../library/std)`
#
# -Zbuild-std resolves the standard library's own workspace, and that
# workspace depends on crates.io packages. The toolchain container has
# no network (CLAUDE.md: containers run with --bridge=none), so the
# fetch has to happen on the host and be mounted in — the same rule
# scripts/fetch-mesa.sh and horizon_ensure_python_deps already follow.
#
# The pin is Rust's own: every name, version and checksum comes from the
# library/Cargo.lock of the rust-src component inside the pinned image,
# and each downloaded .crate is verified against the checksum recorded
# there. Nothing here chooses a version, so nothing here can drift from
# the toolchain — which is the same policy versions.env states for libnx
# and rustc themselves (R15): read the environment, never pin it.
#
# Idempotent: an already-downloaded .crate with a matching checksum is
# left alone, and the vendor directory is only rebuilt for crates that
# changed.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

CRATES_DIR=build/toolchain/rust-crates
VENDOR_DIR=build/toolchain/rust-vendor
LOCKFILE=build/toolchain/rust-library-Cargo.lock
# Where the rust-src component keeps the standard library, inside the
# image. A local rustup install has the same layout under its sysroot.
RUST_SRC_REL=lib/rustlib/src/rust/library

mkdir -p "$CRATES_DIR"

# --- 1. the lockfile, taken from the toolchain rather than committed ---
#
# Committing it would pin the standard library to a version this project
# does not choose; reading it per run keeps the pin where it belongs.
fetch_lockfile() {
    if [ "$HORIZON_IN_CONTAINER" -eq 0 ]; then
        sysroot=$(rustc --print sysroot)
        cp "$sysroot/$RUST_SRC_REL/Cargo.lock" "$LOCKFILE"
        return
    fi
    # docker cp, not `docker run cat`: the container has no network and
    # we want the file on the host, not its contents through a pipe that
    # could swallow an error.
    cid=$(docker create "$HORIZON_IMAGE")
    trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
    src=$(docker run --rm "$HORIZON_IMAGE" sh -c \
              'PATH=/opt/cargo/bin:$PATH rustc --print sysroot' | tr -d '\r')
    docker cp "$cid:$src/$RUST_SRC_REL/Cargo.lock" "$LOCKFILE" >/dev/null
    docker rm -f "$cid" >/dev/null
    trap - EXIT
}

if [ ! -f "$LOCKFILE" ] || [ "$FORCE" -eq 1 ]; then
    echo "fetch-rust-crates: reading library/Cargo.lock from the toolchain"
    fetch_lockfile
fi

# --- 2. the package list: name, version and Rust's own checksum -------
python3 - "$LOCKFILE" "$CRATES_DIR/packages.txt" <<'PY'
import re, sys
lock, out = sys.argv[1], sys.argv[2]
pkgs = []
for blk in open(lock).read().split('[[package]]'):
    n = re.search(r'name = "([^"]+)"', blk)
    v = re.search(r'version = "([^"]+)"', blk)
    s = re.search(r'source = "([^"]+)"', blk)
    c = re.search(r'checksum = "([^"]+)"', blk)
    # Only registry packages need fetching: the rest are path
    # dependencies inside the standard library itself.
    if n and v and s and c and s.group(1).startswith('registry+'):
        pkgs.append((n.group(1), v.group(1), c.group(1)))
open(out, 'w').write(''.join(f"{n} {v} {c}\n" for n, v, c in pkgs))
print(f"fetch-rust-crates: {len(pkgs)} registry packages in the lockfile")
PY

# --- 3. download, verifying each against the lockfile's checksum ------
downloaded=0
kept=0
while read -r name version checksum; do
    crate="$CRATES_DIR/$name-$version.crate"
    if [ -f "$crate" ]; then
        got=$(sha256sum "$crate" | cut -d' ' -f1)
        if [ "$got" = "$checksum" ]; then
            kept=$((kept + 1))
            continue
        fi
        echo "fetch-rust-crates: $name-$version.crate does not match the" \
             "lockfile checksum; re-downloading"
        rm -f "$crate"
    fi
    url="https://static.crates.io/crates/$name/$name-$version.crate"
    curl -fsSL --retry 3 -o "$crate.part" "$url"
    got=$(sha256sum "$crate.part" | cut -d' ' -f1)
    if [ "$got" != "$checksum" ]; then
        rm -f "$crate.part"
        echo "error: $name-$version.crate hashes to $got, but the" >&2
        echo "       lockfile says $checksum. Refusing to vendor it." >&2
        exit 1
    fi
    mv "$crate.part" "$crate"
    downloaded=$((downloaded + 1))
done < "$CRATES_DIR/packages.txt"

echo "fetch-rust-crates: $downloaded downloaded, $kept already current"

# --- 4. extract into a cargo "directory" source ----------------------
#
# Cargo needs .cargo-checksum.json beside each extracted crate: the
# package's own hash, plus a hash per file so a modified vendor tree is
# detected rather than silently used.
python3 - "$CRATES_DIR" "$VENDOR_DIR" <<'PY'
import hashlib, json, os, pathlib, shutil, sys, tarfile
crates, vendor = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
vendor.mkdir(parents=True, exist_ok=True)


def intact(d, chk):
    """Is this vendored crate still exactly what the .crate contained?

    Checking only the recorded package hash is not enough, and that was
    found by breaking it: appending a line to a vendored source file
    left the stamp matching, so the next run reported the tree current.
    Cargo verifies the per-file hashes for crates it actually uses, but
    -Zbuild-std pulls most of this vendor tree in for *resolution* only,
    so a tampered file in an unused crate would never be looked at.
    """
    stamp = d / '.cargo-checksum.json'
    if not stamp.exists():
        return False
    try:
        rec = json.loads(stamp.read_text())
    except ValueError:
        return False
    if rec.get('package') != chk:
        return False
    files = rec.get('files', {})
    on_disk = set()
    for root, _, fs in os.walk(d):
        for f in fs:
            p = pathlib.Path(root) / f
            rel = str(p.relative_to(d))
            if rel == '.cargo-checksum.json':
                continue
            on_disk.add(rel)
            want = files.get(rel)
            if want is None:
                return False
            if hashlib.sha256(p.read_bytes()).hexdigest() != want:
                return False
    return on_disk == set(files)


wanted, n_new, n_repaired = set(), 0, 0
for line in open(crates / 'packages.txt'):
    name, ver, chk = line.split()
    d = vendor / f"{name}-{ver}"
    wanted.add(d.name)
    if intact(d, chk):
        continue
    if d.exists():
        print(f"fetch-rust-crates: {d.name} does not match its checksums;"
              f" re-extracting")
        n_repaired += 1
        shutil.rmtree(d)
    with tarfile.open(crates / f"{name}-{ver}.crate") as t:
        t.extractall(vendor)
    files = {}
    for root, _, fs in os.walk(d):
        for f in fs:
            p = pathlib.Path(root) / f
            files[str(p.relative_to(d))] = \
                hashlib.sha256(p.read_bytes()).hexdigest()
    (d / '.cargo-checksum.json').write_text(
        json.dumps({"files": files, "package": chk}))
    n_new += 1
# A crate dropped from the lockfile must not linger: cargo would still
# find it, and the vendor directory is meant to describe one lockfile.
for d in vendor.iterdir():
    if d.is_dir() and d.name not in wanted:
        print(f"fetch-rust-crates: dropping {d.name} — not in the lockfile")
        shutil.rmtree(d)
print(f"fetch-rust-crates: vendored {n_new} crate(s) "
      f"({n_repaired} re-extracted after a checksum mismatch), "
      f"{len(wanted)} in {vendor}")
PY

# --- 5. the cargo config that points at it ---------------------------
#
# Written rather than committed, because it names an absolute path and
# scripts/check-no-abs-paths.sh forbids one in a tracked file. It lands
# under build/, which is gitignored, and is regenerated per run so a
# moved checkout cannot leave a stale path behind.
CARGO_CFG=build/toolchain/rust-cargo-config.toml
cat > "$CARGO_CFG" <<EOF
# Generated by scripts/fetch-rust-crates.sh — do not edit.
[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "$PWD/$VENDOR_DIR"
EOF

echo "fetch-rust-crates: cargo source replacement written to $CARGO_CFG"
