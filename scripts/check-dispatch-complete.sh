#!/usr/bin/env bash
# Fails when a linked test executable would jump through a NULL Vulkan
# dispatch-table entry.
#
#   scripts/check-dispatch-complete.sh [elf]   # default: the NVK test
#
# WHY THIS EXISTS. Mesa generates its Vulkan dispatch tables with
# --weak, so a driver may leave an entry point unimplemented and the
# linker resolves the gap to zero instead of reporting it. Upstream says
# so itself, and says what to do about it:
#
#   # Instruct users of this library to link with --whole-archive.
#   # Otherwise, our weak function overloads may not resolve properly.
#       -- mesa/src/vulkan/runtime/meson.build:250
#
# Upstream never has to think about it twice, because it links the
# driver into one shared object where every archive is pulled whole by
# construction. Here there is no dynamic loader, the archives are named
# by hand, and a missing one is silent: the link succeeds, `nm -u`
# reports nothing, and the binary carries a NULL function pointer that
# a real call jumps through.
#
# It cost a console round trip:
#
#   ok   vkEnumeratePhysicalDevices(list) -> 0
#   <nothing further, no RESULT line>
#
# vk_common_GetPhysicalDeviceProperties had jumped through
# dispatch_table.GetPhysicalDeviceProperties2, and the binary contained
# no GetPhysicalDeviceProperties2 at all.
#
# WHY IT DOES NOT CHECK FOR WEAK UNDEFINED SYMBOLS, which was the first
# thing tried. devkitA64 links with -z nodynamic-undefined-weak
# (libnx's switch.specs), so the linker resolves such a symbol to zero
# and it does not survive into the symbol table. A gate reading `nm`
# for them reports OK on a binary built with the bug still in it —
# measured, on a file written to have exactly that hole. A gate that
# cannot fail is not a gate.
#
# WHAT IT CHECKS INSTEAD. Every entry point the generated common
# dispatch table names must be implemented by *something* in the
# binary: nvk_<Entry>, vk_common_<Entry>, or the KHR/EXT alias a
# promoted core entry point is often implemented under. Only core entry
# points are required — an unimplemented extension entry point is a
# legitimate NULL, because the extension is not advertised.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# shellcheck source=toolchain-env.sh
. scripts/toolchain-env.sh

# With no argument: every ELF in the build directory that links the
# driver, not just t_vulkan.
#
# The hole this gate exists to catch is per-binary — it depends on which
# archives that link line pulled — so checking one binary says nothing
# about the others. While t_vulkan was the only such test that
# distinction did not exist; Phase 5 adds six more, and a default that
# still named one of them would report OK for a set it never looked at.
#
# `set --` rather than a loop here: the body below checks exactly one
# ELF, so the whole script re-executes itself once per file. That keeps
# the single-file path — which is what a caller passing an argument
# gets, and what the loop needs — as the only code path there is.
if [ "$#" -eq 0 ]; then
    set -- "$HORIZON_BUILD_DIR"/t_vulkan.elf "$HORIZON_BUILD_DIR"/t_vk_*.elf
    found=0
    for e in "$@"; do
        [ -f "$e" ] || continue
        found=$((found + 1))
        "$0" "$e" || exit 1
    done
    if [ "$found" -eq 0 ]; then
        echo "check-dispatch-complete: no driver-linking ELF in" \
             "$HORIZON_BUILD_DIR; nothing to check"
    fi
    exit 0
fi

ELF="$1"
TABLE="$NVK_BUILD_DIR_DEFAULT/src/vulkan/runtime/vk_common_entrypoints.c"

if [ ! -f "$ELF" ]; then
    echo "check-dispatch-complete: $ELF is not built; nothing to check"
    exit 0
fi
if [ ! -f "$TABLE" ]; then
    echo "check-dispatch-complete: $TABLE is not generated; nothing to" \
         "check"
    exit 0
fi

work=build/toolchain/dispatch-check
mkdir -p "$work"
horizon_run "${DEVKITA64_TOOL_PREFIX}nm" "$ELF" > "$work/nm.txt" 2>/dev/null

python3 - "$TABLE" "$work/nm.txt" <<'PY'
import re, sys

table, nmfile = sys.argv[1], sys.argv[2]

entries = sorted(set(re.findall(r'=\s*(vk_common_[A-Za-z0-9_]+)',
                                open(table).read())))

defined = set()
for line in open(nmfile):
    parts = line.split()
    # "<addr> T name" for a definition; "U name" has no address.
    if len(parts) == 3 and parts[1] in 'TtWwDdBbRr':
        defined.add(parts[2])

VENDOR = ('KHR', 'EXT', 'NV', 'NVX', 'AMD', 'INTEL', 'ANDROID', 'GOOGLE',
          'FUCHSIA', 'QCOM', 'ARM', 'HUAWEI', 'VALVE', 'MESA', 'IMG',
          'SEC', 'NN', 'MVK', 'QNX', 'AMDX', 'GGP', 'OHOS')

# The one core entry point nothing here implements, and why that is
# right rather than an oversight: enumerating instance layers is the
# loader's job, and this platform has no loader. Nothing can call it —
# there is no trampoline for it in the binary either — so a NULL in
# that slot is unreachable. Listed rather than silently filtered.
ALLOWED = {
    'EnumerateInstanceLayerProperties':
        'layer enumeration belongs to the Vulkan loader, and Horizon '
        'has none',
}

missing = []
for sym in entries:
    ep = sym[len('vk_common_'):]
    if any(ep.endswith(v) for v in VENDOR):
        continue                       # extension: a NULL is legitimate
    if ep in ALLOWED:
        continue
    names = [f'vk_common_{ep}', f'nvk_{ep}']
    for v in ('KHR', 'EXT'):
        names += [f'vk_common_{ep}{v}', f'nvk_{ep}{v}']
    if not any(n in defined for n in names):
        missing.append(ep)

if missing:
    print('error: %d core Vulkan entry point(s) are named by the dispatch'
          % len(missing), file=sys.stderr)
    print('       table and implemented by nothing in the binary. Each is'
          , file=sys.stderr)
    print('       a NULL pointer at run time and a jump to address zero'
          , file=sys.stderr)
    print('       when something calls it. An archive is probably missing'
          , file=sys.stderr)
    print('       from the link, or linked without --whole-archive.'
          , file=sys.stderr)
    for ep in missing:
        print('         ' + ep, file=sys.stderr)
    raise SystemExit(1)

print('check-dispatch-complete: OK (%d entry points named, %d core, '
      '%d allowed absence)'
      % (len(entries),
         len([e for e in entries
              if not any(e[len('vk_common_'):].endswith(v) for v in VENDOR)]),
         len(ALLOWED)))
PY
