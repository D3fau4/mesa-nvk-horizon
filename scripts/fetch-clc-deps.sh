#!/usr/bin/env bash
# Fetches, on the host, what the toolchain image needs in order to
# build Mesa's OpenCL-C compiler — `mesa_clc` — natively.
#
#   scripts/fetch-clc-deps.sh [--force]
#
# WHY mesa_clc IS NEEDED AT ALL. NVK compiles two OpenCL C files into
# SPIR-V at *build* time and turns the result into C++:
#
#   src/nouveau/vulkan/cl/nvk_query.cl          -> nvkcl.spv -> nvkcl.cpp
#   src/nouveau/vulkan/cl/nvk_copy_indirect.cl
#
# (mesa/src/nouveau/vulkan/meson.build:99-118). They implement query-pool
# copies and indirect copies, so they are not optional, and mesa/meson.build
# puts with_nouveau_vk in `with_driver_using_cl` unconditionally. There is
# no configuration of NVK that does not need them.
#
# mesa_clc is a *build-machine* tool. It needs LLVM, clang,
# SPIRV-LLVM-Translator, SPIRV-Tools and libclc — none of which the base
# image has, and none of which a container here can install for itself.
# Same split as scripts/fetch-rust-tools.sh: fetch on the host, build in
# the image.
#
# TWO SOURCES, FOR ONE REASON EACH:
#
#   Debian bookworm packages — LLVM 15 rather than the 16 already in the
#     image for libclang. Mesa requires an SPIRV-LLVM-Translator whose
#     version matches the chosen LLVM major.minor (mesa/meson.build:2038),
#     and bookworm packages libllvmspirvlib for 14 and 15 only. Choosing
#     LLVM 15 means everything comes from the distribution; choosing 16
#     would mean building the translator from source as well.
#
#   Source build — SPIRV-Tools. Mesa requires >= 2024.1
#     (mesa/meson.build:2054) and bookworm ships 2023.1. This is the one
#     piece the distribution cannot supply, so it is built in the image
#     from a pinned release tarball.
#
# The .deb dependency closure is resolved here, from Debian's own
# Packages index, against the set of packages the base image already
# has — so the image gains exactly what is missing and not a whole
# distribution. Every file is verified against the SHA256 the index
# records.
#
# Idempotent: an already-downloaded file with a matching hash is left
# alone. --force re-resolves and re-downloads.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

DL=build/toolchain/clc-deps
STAMP="$DL/.stamp"

identity() {
    echo "roots=$DEBIAN_CLC_PACKAGES"
    echo "suite=$DEBIAN_SUITE"
    echo "spirv-tools=$SPIRV_TOOLS_COMMIT"
    echo "spirv-headers=$SPIRV_HEADERS_COMMIT"
    echo "base=$(docker image inspect --format '{{.Id}}' \
                     "$HORIZON_BASE_IMAGE" 2>/dev/null || echo unknown)"
}

if [ "$FORCE" -eq 0 ] && [ -f "$STAMP" ] &&
   [ "$(identity)" = "$(cat "$STAMP")" ]; then
    echo "fetch-clc-deps: $DL is current" \
         "($(find "$DL" -name '*.deb' | wc -l) packages)"
    exit 0
fi

mkdir -p "$DL/debs" "$DL/src"

# --- 1. what the base image already has -------------------------------
#
# Without this the closure below would pull in libc6 and half a
# distribution. Asked of the image itself rather than assumed, because
# the base image is a floating tag and its contents are the
# environment's business (toolchain/versions.env).
# The *versions* matter, not just the names. A dependency like
#   libc6-i386 depends on libc6 (= 2.36-9+deb12u14)
# is not satisfied by the 2.36-9+deb12u10 the image happens to carry,
# and treating "libc6 is installed" as an answer produced a closure
# that dpkg then refused to install — five packages left unconfigured,
# with the real reason four screens up. So each package is recorded
# with its version and every constraint is checked against it.
if [ "$HORIZON_IN_CONTAINER" -eq 1 ]; then
    echo "fetch-clc-deps: asking $HORIZON_BASE_IMAGE what it already has"
    docker run --rm "$HORIZON_BASE_IMAGE" \
        dpkg-query -W -f '${Package} ${Version}\n${Provides}\n' 2>/dev/null |
        sed 's/,/\n/g' | sed 's/^ *//;s/ *$//' | grep . |
        sort -u > "$DL/installed.txt"
else
    : > "$DL/installed.txt"
fi

# --- 2. Debian's package index ----------------------------------------
echo "fetch-clc-deps: fetching the $DEBIAN_SUITE package index"
curl -fsSL --retry 3 -o "$DL/Packages.gz" \
    "$DEBIAN_MIRROR/dists/$DEBIAN_SUITE/main/binary-amd64/Packages.gz"

# --- 3. resolve the closure, then download and verify ------------------
python3 - "$DL" "$DEBIAN_MIRROR" "$DEBIAN_CLC_PACKAGES" <<'PY'
import gzip, hashlib, os, pathlib, re, subprocess, sys, urllib.request

dl, mirror, roots = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3].split()

# One record per binary package, keyed by name and by anything it
# Provides — a dependency may name a virtual package.
pkgs, provides = {}, {}
with gzip.open(dl / 'Packages.gz', 'rt', encoding='utf-8') as fh:
    for block in fh.read().split('\n\n'):
        if not block.strip():
            continue
        rec = {}
        key = None
        for line in block.split('\n'):
            if line[:1] in (' ', '\t') and key:
                rec[key] += '\n' + line.strip()
            elif ':' in line:
                key, _, val = line.partition(':')
                rec[key] = val.strip()
        name = rec.get('Package')
        if not name:
            continue
        # Later records win; the index is sorted so this keeps the
        # highest version of a package listed more than once.
        pkgs[name] = rec
        for p in rec.get('Provides', '').split(','):
            p = p.split('(')[0].strip().split(':', 1)[0]
            if p:
                provides.setdefault(p, name)

# What the image has, name -> version. A "name" with no version is a
# virtual package from a Provides field; those carry no version to
# check against, so they satisfy any constraint.
have = {}
for line in (dl / 'installed.txt').read_text().splitlines():
    parts = line.split()
    if parts:
        have[parts[0].split(':', 1)[0]] = parts[1] if len(parts) > 1 else None


def satisfied_by_installed(name, op, ver):
    """Is `name (op ver)` met by what the image already has?"""
    if name not in have:
        return False
    if op is None:
        return True
    installed = have[name]
    if installed is None:      # virtual package, no version to compare
        return False
    # dpkg's own comparison rather than a reimplementation of it:
    # Debian versions have epochs, tildes and mixed alphanumeric
    # segments, and getting that subtly wrong is how a closure ends up
    # looking right and failing at install time.
    return subprocess.run(['dpkg', '--compare-versions', installed, op, ver],
                          check=False).returncode == 0


_DEP_RE = re.compile(r'^([^ (]+)(?:\s*\(\s*([<>=]+)\s*([^)]+)\))?')


def parse_alt(alt):
    m = _DEP_RE.match(alt.strip())
    if not m:
        return None, None, None
    name, op, ver = m.group(1), m.group(2), m.group(3)
    # Strip the architecture qualifier: `python3:any`, `libfoo:native`,
    # `libbar:amd64`. The index keys packages by bare name, so leaving
    # it on made a perfectly ordinary dependency look unsatisfiable —
    #   error: not in the index and not in the image: python3:any
    # — for a package the image has had all along.
    name = name.split(':', 1)[0]
    # dpkg spells strict/loose comparison differently from the control
    # file syntax: << and >> are strict, <= and >= loose, and a bare <
    # or > is deprecated but means the loose form.
    if op in ('<', '>'):
        op += '='
    return name, op, ver


# A Depends field is a comma-separated list of alternatives joined by
# '|', each possibly with a version constraint. An alternative already
# satisfied by the image wins outright — that is what keeps the closure
# to what is missing rather than to a whole distribution. Otherwise the
# first alternative the index can supply is taken: apt's own
# preference, and picking a later one would need policy this script has
# no way to express.
def resolve_clause(clause):
    alts = [parse_alt(a) for a in clause.split('|')]
    for name, op, ver in alts:
        if name and satisfied_by_installed(name, op, ver):
            return None                      # nothing to fetch
    for name, op, ver in alts:
        if name and (name in pkgs or name in provides):
            return name
    return alts[0][0] if alts and alts[0][0] else None


def deps_of(rec):
    out = []
    for clause in rec.get('Depends', '').split(','):
        if not clause.strip():
            continue
        name = resolve_clause(clause)
        if name:
            out.append(name)
    return out


want, queue, missing = {}, list(roots), []
while queue:
    name = queue.pop()
    if name in want:
        continue
    rec = pkgs.get(name) or pkgs.get(provides.get(name, ''))
    if rec is None:
        missing.append(name)
        continue
    if rec['Package'] in want:
        continue
    want[rec['Package']] = rec
    queue.extend(deps_of(rec))

if missing:
    print('error: not in the index and not in the image: '
          + ', '.join(sorted(set(missing))), file=sys.stderr)
    raise SystemExit(1)

debs = dl / 'debs'
for f in debs.iterdir():
    if f.suffix == '.deb' and f.name not in {
            os.path.basename(r['Filename']) for r in want.values()}:
        print(f'fetch-clc-deps: dropping {f.name} — no longer in the closure')
        f.unlink()

total = 0
for name in sorted(want):
    rec = want[name]
    out = debs / os.path.basename(rec['Filename'])
    want_sha = rec['SHA256']
    if out.exists() and hashlib.sha256(out.read_bytes()).hexdigest() == want_sha:
        total += 1
        continue
    print(f'fetch-clc-deps: downloading {name} {rec["Version"]}')
    data = urllib.request.urlopen(f'{mirror}/{rec["Filename"]}').read()
    got = hashlib.sha256(data).hexdigest()
    if got != want_sha:
        print(f'error: {name} hashes to {got}, index says {want_sha}',
              file=sys.stderr)
        raise SystemExit(1)
    out.write_bytes(data)
    total += 1

# A record of exactly what was resolved, so a later run that produces a
# different set is visible as a diff rather than as a silently
# different image.
(dl / 'closure.txt').write_text(
    ''.join(f'{n} {want[n]["Version"]} {want[n]["SHA256"]}\n'
            for n in sorted(want)))
print(f'fetch-clc-deps: {total} package(s) in the closure')
PY

# --- 4. SPIRV-Tools, which the distribution cannot supply -------------
#
# Cloned rather than downloaded as a release tarball:
# codeload.github.com answers 403 through this environment's egress
# proxy, while git-over-https to the same host is allowed. Measured.
#
# The tag is fetched and then the resolved commit is *checked*, for the
# same reason scripts/fetch-mesa.sh checks Mesa's: a tag can be moved.
# The .git directory is removed afterwards — the image build wants
# sources, not history, and it keeps the context small.
fetch_repo() { # dest repo tag commit
    _dest="$DL/src/$1"
    if [ -f "$_dest/.mesa-nvk-horizon-commit" ] &&
       [ "$(cat "$_dest/.mesa-nvk-horizon-commit")" = "$4" ]; then
        return 0
    fi
    echo "fetch-clc-deps: cloning $1 at $3"
    rm -rf "$_dest" "$_dest.tmp"
    git clone --depth 1 --quiet --branch "$3" "$2" "$_dest.tmp"
    _got=$(git -C "$_dest.tmp" rev-parse HEAD)
    if [ "$_got" != "$4" ]; then
        rm -rf "$_dest.tmp"
        echo "error: $1 $3 resolved to $_got, but" >&2
        echo "       toolchain/versions.env pins $4. The tag moved, or" >&2
        echo "       the pin is wrong. Refusing to continue." >&2
        exit 1
    fi
    rm -rf "$_dest.tmp/.git"
    printf '%s\n' "$4" > "$_dest.tmp/.mesa-nvk-horizon-commit"
    mv "$_dest.tmp" "$_dest"
}

fetch_repo SPIRV-Tools "$SPIRV_TOOLS_REPO" "$SPIRV_TOOLS_TAG" \
           "$SPIRV_TOOLS_COMMIT"
# SPIRV-Headers has no tag for the revision SPIRV-Tools' DEPS pins, so
# it is cloned by commit. --depth 1 cannot do that on every server, so
# this one fetches the ref and checks it out explicitly.
if [ ! -f "$DL/src/SPIRV-Headers/.mesa-nvk-horizon-commit" ] ||
   [ "$(cat "$DL/src/SPIRV-Headers/.mesa-nvk-horizon-commit")" != \
     "$SPIRV_HEADERS_COMMIT" ]; then
    echo "fetch-clc-deps: cloning SPIRV-Headers at $SPIRV_HEADERS_COMMIT"
    rm -rf "$DL/src/SPIRV-Headers" "$DL/src/SPIRV-Headers.tmp"
    git init -q "$DL/src/SPIRV-Headers.tmp"
    git -C "$DL/src/SPIRV-Headers.tmp" remote add origin "$SPIRV_HEADERS_REPO"
    git -C "$DL/src/SPIRV-Headers.tmp" fetch -q --depth 1 origin \
        "$SPIRV_HEADERS_COMMIT"
    git -C "$DL/src/SPIRV-Headers.tmp" checkout -q --detach FETCH_HEAD
    rm -rf "$DL/src/SPIRV-Headers.tmp/.git"
    printf '%s\n' "$SPIRV_HEADERS_COMMIT" \
        > "$DL/src/SPIRV-Headers.tmp/.mesa-nvk-horizon-commit"
    mv "$DL/src/SPIRV-Headers.tmp" "$DL/src/SPIRV-Headers"
fi

identity > "$STAMP"
echo "fetch-clc-deps: $DL is ready;" \
     "scripts/build-toolchain-image.sh installs and builds it"
