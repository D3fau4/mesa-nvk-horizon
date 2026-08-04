#!/usr/bin/env bash
# Parity gate for the facts tests 12 and 13 need, which four files state
# separately.
#
#   scripts/check-mesa-test-parity.sh
#
# WHY THIS EXISTS. The Makefile and meson.build each describe the two
# Mesa tests in full — which tests they are, which archives they link,
# which defines and include paths they compile with — and
# scripts/toolchain-env.sh and meson.options each carry a piece of the
# same description. Until this script they were held together by a
# comment saying "kept in step with the Makefile", and the existing
# parity check compares .nro *sizes*, which would not notice a define
# present on one path and missing on the other: it would notice only if
# the resulting binaries happened to differ in length.
#
# The duplication itself is deliberate. Each build system stays readable
# and usable on its own — the Makefile is the path whose output was
# verified on real hardware and must not need a script to be understood
# — so the answer is to make divergence fail rather than to invent a
# shared file neither build system can read natively.
#
# WHAT THIS IS NOT. It compares what the build files *say*, not what the
# compiler was handed. Comparing the generated commands would be
# stronger; it also requires both paths to be configured and built,
# which a bare clone is not. This runs anywhere, needs no toolchain, and
# catches the failure that actually happens: an edit to one build system
# and not the other.
#
# Exit code 0 = the four facts agree, 1 = they do not (printed).
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.."

fail=0

# Everything is compared as a sorted set of tokens, so brackets, quotes,
# commas, line continuations and ordering are not differences. Anything
# that is not part of a path, a name or a flag separates tokens.
tokens() {
    tr -c 'A-Za-z0-9_./=-' '\n' | grep -v '^$' | LC_ALL=C sort -u |
        tr '\n' ' '
}

# Reports whether every source agrees. Arguments are label, then
# "where:value" pairs.
compare() {
    local what="$1" first="" first_where="" pair where value
    shift

    for pair in "$@"; do
        where=${pair%%:*}
        value=${pair#*:}

        if [ -z "$(printf '%s' "$value" | tr -d ' ')" ]; then
            echo "MESA TEST PARITY ($what): extracted nothing from $where."
            echo "  The extraction in this script no longer matches that"
            echo "  file. Fix the extraction — an empty set must never be"
            echo "  reported as agreement."
            fail=1
            return 0
        fi

        if [ -z "$first_where" ]; then
            first=$value
            first_where=$where
        elif [ "$value" != "$first" ]; then
            echo "MESA TEST PARITY ($what): $first_where and $where disagree."
            echo "  $first_where: $first"
            echo "  $where: $value"
            fail=1
        fi
    done
    return 0
}

# 1. Which tests are the Mesa ones.
mk_tests=$(sed -n 's/^MESA_TESTS  *:*= *//p' Makefile | tokens)
ms_tests=$(sed -n 's/^mesa_tests  *= *//p' meson.build | tokens)
compare "test names" "Makefile:$mk_tests" "meson.build:$ms_tests"

# 2. Which archives they link, relative to the Mesa build directory. The
#    prefixes are stripped first so the two spellings of "inside
#    $MESA_BUILD_DIR" do not read as a difference.
mk_libs=$(sed -n '/^MESA_LIBS/,/[^\\]$/p' Makefile |
          sed 's/^MESA_LIBS  *:*= *//; s|\$(MESA_BUILD)/||g' | tokens)
ms_libs=$(sed -n '/^mesa_test_libs  *= *\[/,/^\]/p' meson.build |
          sed '1d; $d; s|mesa_build_dir  */  *||g' | tokens)
sh_libs=$(sed -n 's/^HORIZON_MESA_TEST_LIBS=//p' scripts/toolchain-env.sh |
          tokens)
compare "archive paths" "Makefile:$mk_libs" "meson.build:$ms_libs" \
        "scripts/toolchain-env.sh:$sh_libs"

# 3. The defines Mesa's own configure decided here and both paths copy.
#    A missing one changes which types the headers declare, and the
#    result still links.
mk_defs=$(sed -n 's/^MESA_CFLAGS  *:*= *//p' Makefile |
          grep -o -- '-D[A-Za-z_][A-Za-z0-9_]*' | tokens)
ms_defs=$(grep -o -- "'-D[A-Za-z_][A-Za-z0-9_]*'" meson.build | tokens)
compare "defines" "Makefile:$mk_defs" "meson.build:$ms_defs"

# 4. Mesa's include paths.
mk_incs=$(sed -n 's/^MESA_CFLAGS  *:*= *//p' Makefile |
          grep -o -- '-Imesa/[A-Za-z0-9_/]*' | sed 's/^-I//' | tokens)
ms_incs=$(sed -n "s/.*include_directories(\('mesa.*\)).*/\1/p" meson.build |
          tokens)
compare "mesa include dirs" "Makefile:$mk_incs" "meson.build:$ms_incs"

# 5. The default Mesa build directory. One literal, in three places: a
#    caller who sets none must get the same directory from all of them,
#    or one build path skips the two tests while the other builds them.
mk_dir=$(sed -n 's/^MESA_BUILD  *:*= *\$(or \$(MESA_BUILD_DIR),\(.*\))$/\1/p' \
         Makefile | tokens)
sh_dir=$(sed -n 's/^MESA_BUILD_DIR="\${MESA_BUILD_DIR:-\(.*\)}"$/\1/p' \
         scripts/toolchain-env.sh | tokens)
# Scoped to the mesa_build_dir option, not to every option that has a
# string value. It used to take them all, which was right while
# meson.options had one such option and wrong the moment a second
# arrived: nvk_build_dir made this read "build/mesa-nvk build/mesa-probe"
# and the check failed on a difference that was not one.
op_dir=$(sed -n "/^ *'mesa_build_dir',/,/^)/{ s/^ *value *: *'\(.*\)',*$/\1/p; }" \
         meson.options | tokens)
compare "default mesa build dir" "Makefile:$mk_dir" \
        "scripts/toolchain-env.sh:$sh_dir" "meson.options:$op_dir"

if [ "$fail" -eq 0 ]; then
    echo "check-mesa-test-parity: OK (tests, archives, defines, includes," \
         "default build dir)"
fi
exit "$fail"
