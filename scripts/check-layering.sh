#!/usr/bin/env bash
# Layering gate for horizon/ (milestones.md Phase 1 exit criterion;
# CLAUDE.md layer rules): nothing under horizon/ may include a Vulkan,
# Mesa or nwindow/vi header, reference nwindow symbols, or reintroduce
# the banned global-device pattern.
#
# Exit code 0 = clean, 1 = violation (printed).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.."

fail=0

report() {
    echo "LAYERING VIOLATION ($1):"
    echo "$2"
    fail=1
}

# 1. Forbidden includes: Vulkan, Mesa trees, DRM uAPI, nwindow/vi/display.
pattern='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([Vv]ulkan|vk_|vulkan/|nir|compiler/|util/|mesa|drm|xf86drm|libdrm|switch/display/|nwindow)'
out=$(grep -RInE "$pattern" horizon/ 2>/dev/null)
[ -n "$out" ] && report "forbidden include" "$out"

# 2. nwindow / vi queueing symbols (presentation is WSI's job,
#    architecture.md §3).
out=$(grep -RInE 'nwindow[A-Za-z]*\(|viCreateLayer|viOpenDisplay' horizon/ 2>/dev/null)
[ -n "$out" ] && report "nwindow/vi usage" "$out"

# 3. Banned global-device pattern (CLAUDE.md rejected design 5).
out=$(grep -RInE '(^|[^A-Za-z0-9_])g_(dev|device|swapchain|window|channel)[^A-Za-z0-9_]' horizon/ 2>/dev/null)
[ -n "$out" ] && report "global device state" "$out"

# 4. Submit-path CPU waits (CLAUDE.md rejected design 6): nvFenceWait may
#    appear only in the sync/ and channel/ wait implementations.
#    Comment lines (' *', '//', '/*') are excluded — the ban is on calls.
out=$(grep -RInE 'nvFenceWait|nvMultiFenceWait' horizon/ 2>/dev/null |
      grep -vE '^horizon/(sync|channel)/' |
      grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)')
[ -n "$out" ] && report "CPU wait outside sync/channel" "$out"

# 5. horizon/include/ is the public API surface nvkmd_horizon consumes; the
#    headers themselves claim to be libnx-free (CLAUDE.md layer rules,
#    architecture.md § 3) — check that claim in code, not just doc
#    comments. Comment lines are excluded from the type-usage grep since
#    several headers cite libnx names (NvKind, Result) descriptively.
pattern='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]switch'
out=$(grep -RInE "$pattern" horizon/include/ 2>/dev/null)
[ -n "$out" ] && report "libnx header included from horizon/include/" "$out"
out=$(grep -RInE '\bNv[A-Z][A-Za-z]*\b|\bResult\b' horizon/include/ 2>/dev/null |
      grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)')
[ -n "$out" ] && report "libnx type used in horizon/include/" "$out"

# 6-8. Rejected designs 1-3 (CLAUDE.md): no simulated /dev/dri device, no
#    libc interposition via -Wl,--wrap, no nouveau DRM uAPI
#    reimplementation. Scoped to our own code, not docs/ (which names
#    these patterns to explain why the reference ports are not the model)
#    or mesa/ (a pinned upstream checkout, not ours to police here).
GREP_EXCL=(--exclude=check-layering.sh)
out=$(grep -RInE "${GREP_EXCL[@]}" '/dev/dri' horizon/ tests/ scripts/ Makefile 2>/dev/null)
[ -n "$out" ] && report "simulated /dev/dri (rejected design 1)" "$out"
out=$(grep -RInE "${GREP_EXCL[@]}" -- '--wrap' horizon/ tests/ scripts/ Makefile 2>/dev/null)
[ -n "$out" ] && report "libc --wrap interposition (rejected design 2)" "$out"
out=$(grep -RInE "${GREP_EXCL[@]}" \
      'drm_nouveau_(gem_new|exec|vm_bind)|drmSyncobj[A-Za-z]*' \
      horizon/ tests/ scripts/ Makefile 2>/dev/null)
[ -n "$out" ] && report "nouveau DRM uAPI symbol (rejected design 3)" "$out"

if [ "$fail" -eq 0 ]; then
    echo "check-layering: OK (horizon/ is Vulkan-, Mesa- and nwindow-free)"
fi
exit "$fail"
