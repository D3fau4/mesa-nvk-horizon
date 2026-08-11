#!/usr/bin/env bash
# Prints the driver identity this port hands to Mesa, on stdout.
#
#   scripts/gen-driver-id.sh
#
# WHY IT IS NOT gen-build-id.sh, WHICH ALREADY STAMPS BUILDS. Because
# they answer opposite questions and want opposite properties.
# gen-build-id.sh exists to tell two builds apart and is therefore a
# timestamp: two builds of identical source ARE different artefacts once
# one of them is the .nro on the SD card. This one exists to tell two
# *drivers* apart, and must be the same for identical source — otherwise
# every rebuild would throw away a shader cache that is still perfectly
# valid, and reproducible builds would stop being reproducible.
#
# WHY IT EXISTS AT ALL. Horizon has no dynamic loader, so NVK cannot read
# an ELF build id and mesa-patches/0018 derives the driver identity from
# PACKAGE_VERSION and MESA_GIT_SHA1 instead. Both describe the pinned
# Mesa checkout. Neither describes mesa-patches/ or horizon/, which
# together are most of this driver and contain the whole compiler backend
# and the whole platform layer. While that identity was only driverUUID
# the cost of it being stale was an application reusing its own blob.
# With a shader disk cache reading it, the cost is last build's binaries
# handed to this build's hardware, and that has no error path.
#
# So the identity is a digest of everything that can change generated
# code:
#
#   - the pinned Mesa commit (toolchain/versions.env)
#   - every patch in mesa-patches/, by content
#   - every source and header under horizon/, by content
#   - every source under compat/, which is linked into the driver too
#   - toolchain/versions.env whole, which pins the toolchain image and
#     therefore the compiler, the Rust toolchain and the linker
#   - toolchain/*.cross, which carries the code-generation flags:
#     -march, -mtune, -mtp, -fPIE, the lot
#
# and of nothing else. Editing a comment in a document or a test does not
# change it, which is the intended behaviour: neither can change a shader
# binary.
#
# WHAT IT STILL DOES NOT COVER, said plainly because a reviewer asked and
# the answer is "less than everything". The digest describes the inputs
# this repository *names*. It cannot describe a toolchain somebody
# substituted without telling it: a local $DEVKITPRO whose gcc differs
# from the pinned image's, or an image republished under the same tag.
# In both cases the sources, the flags and versions.env are identical
# and the compiler is not, so the identity does not move and a stale
# shader could be served.
#
# Closing that would mean hashing the built driver, or running the cross
# compiler to ask its version — the first inverts the dependency (the id
# is needed to configure the build that produces the binary) and the
# second puts a container round-trip in every configure. Neither is worth
# it for a hazard that requires deliberately building against an
# unpinned toolchain. It is recorded here rather than pretended away, and
# `scripts/print-toolchain-versions.sh` is what a build should be
# reported with.
#
# Passed to Mesa as -Dhorizon-driver-id by scripts/configure-mesa.sh and
# scripts/configure-mesa-nvk.sh.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

# MESA_COMMIT comes from toolchain/versions.env through toolchain-env.sh.
# It is named rather than read out of mesa/, so that a checkout somebody
# has committed on top of does not silently change the identity: the pin
# is what this port claims to be built against.
{
    echo "mesa-commit ${MESA_COMMIT}"

    # Sorted so the digest does not depend on the filesystem's order.
    # `--` and `-print0` are not needed here (these paths are ours and
    # contain no whitespace) but LC_ALL is: sort order is locale-dependent
    # and this value has to be reproducible on somebody else's machine.
    find mesa-patches -name '*.patch' -type f | LC_ALL=C sort |
        while IFS= read -r f; do
            printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"
        done

    find horizon compat -type f \( -name '*.c' -o -name '*.h' \) |
        LC_ALL=C sort |
        while IFS= read -r f; do
            printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"
        done

    # The toolchain pin and the code-generation flags. Only tracked
    # files: build/toolchain/devkitpro.cross is generated and carries a
    # machine-specific prefix, so hashing it would make the identity —
    # and therefore every cache key — differ between two machines
    # building the same driver.
    find toolchain -maxdepth 1 -type f \
         \( -name 'versions.env' -o -name '*.cross' \) |
        LC_ALL=C sort |
        while IFS= read -r f; do
            printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"
        done
} | sha256sum | cut -d' ' -f1
