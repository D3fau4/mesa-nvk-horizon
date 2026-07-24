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

if [ "$fail" -eq 0 ]; then
    echo "check-layering: OK (horizon/ is Vulkan-, Mesa- and nwindow-free)"
fi
exit "$fail"
