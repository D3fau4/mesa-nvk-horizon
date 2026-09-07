#!/usr/bin/env bash
# Parity gate for the facts about the test suites that four files state
# separately.
#
#   scripts/check-mesa-test-parity.sh
#
# WHY THIS EXISTS. The Makefile and meson.build each describe the suites
# in full — which suites they are, which cases each holds, which
# archives they link, which defines and include paths they compile with
# — and scripts/toolchain-env.sh and meson.options each carry a piece of
# the same description. Until this script they were held together by a
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

# The value of one Makefile variable, line continuations folded and the
# assignment stripped. A sed range would not do: `/^NAME.*=/,/[^\\]$/`
# always takes at least two lines, so a single-line definition swallows
# whatever follows it — which is how the first version of this read
# `SUITES := platform …` and came back with the CASES_ lines as well.
# `+=` is deliberately not matched: the Mesa suites are appended to
# SUITES under a condition, and comparison 1 covers them separately.
make_var() { # variable name -> its value on one line
    awk -v name="$1" '
        active == 0 {
            if ($0 !~ "^" name "[ \t]*:?=[ \t]*")
                next
            sub("^" name "[ \t]*:?=[ \t]*", "")
            active = 1
        }
        {
            if (sub(/\\[ \t]*$/, "")) {
                printf "%s ", $0
            } else {
                printf "%s\n", $0
                exit
            }
        }' Makefile
}

# The keys of one of meson.build's suite dictionaries. A key is at a
# two-space indent and a case name never is, so the two do not collide.
meson_suite_names() { # dict name -> its suite names
    sed -n "/^$1 = {/,/^}/p" meson.build | sed 's/#.*//' |
        grep -o "^  '[a-z_0-9]*' *:" | tr -d " ':" | LC_ALL=C sort -u |
        tr '\n' ' '
}

# The cases one suite lists, from that same dictionary. An entry may span
# lines, so it is accumulated from the key to the closing bracket and the
# key itself dropped before the names are read out.
meson_suite_cases() { # dict name, suite -> its case names
    sed -n "/^$1 = {/,/^}/p" meson.build | sed 's/#.*//' |
        awk -v key="'$2'" '
            $0 ~ "^  " key " *:" { inside = 1 }
            inside {
                buf = buf $0
                if (index($0, "]")) {
                    sub(/^[^[]*\[/, "", buf)
                    print buf
                    exit
                }
            }' |
        grep -o "'[^']*'" | tr -d "'" | LC_ALL=C sort -u | tr '\n' ' '
}

# 1. Which suites are the Mesa ones.
mk_tests=$(make_var MESA_SUITES | tokens)
ms_tests=$(meson_suite_names mesa_suites | tokens)
compare "mesa suite names" "Makefile:$mk_tests" "meson.build:$ms_tests"

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
#
#    The meson side is read from idep_mesa_core's compile_args and NOT
#    from every -D in the file. It used to be the latter, which was
#    right while tests 12 and 13 were the only ones with defines of
#    their own: Phase 6 gave the NVK tests -DVK_USE_PLATFORM_VI_NN
#    through idep_nvk_driver, and a file-wide grep then reported a
#    disagreement between the Makefile and meson.build about a define
#    that has nothing to do with either of the two tests this gate is
#    about. The extraction is narrowed to the dependency those tests
#    actually carry.
mk_defs=$(sed -n 's/^MESA_CFLAGS  *:*= *//p' Makefile |
          grep -o -- '-D[A-Za-z_][A-Za-z0-9_]*' | tokens)
ms_defs=$(sed -n '/idep_mesa_core = declare_dependency(/,/^  )/p' meson.build |
          grep -o -- "'-D[A-Za-z_][A-Za-z0-9_]*'" | tokens)
compare "defines" "Makefile:$mk_defs" "meson.build:$ms_defs"

# AND THE NARROWING IS DECLARED, not silent. Review of PR #8 called this
# "narrowed to silence a real difference", and the scope needs saying out
# loud for that to be answerable: the two build paths do not build the
# same set of tests. The Makefile builds the two that link Mesa's core
# archives; meson also builds the NVK tests, which carry defines of their
# own through idep_nvk_driver. Those are out of this gate's scope because
# the Makefile has nothing to apply them to — but "out of scope" is a
# claim, so it is printed and can be checked by eye.
nvk_only=$(sed -n "/idep_nvk_driver = declare_dependency(/,/^  )/p" meson.build |
           grep -o -- "'-D[A-Za-z_][A-Za-z0-9_]*'" | tokens)
if [ -n "$nvk_only" ]; then
    echo "check-mesa-test-parity: NOT compared — defines that reach only the" \
         "NVK tests, which the Makefile path does not build: $nvk_only"
fi

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

# 6. The archives the NVK tests link. Only two files state this one —
#    there is no Makefile path for the NVK tests — but the two must
#    agree for a reason the sentinel version of this list demonstrated:
#    horizon_nvk_libs_present() is what the scripts use to predict
#    meson.build's fs.exists() answer, and a shorter list makes it
#    predict "yes" while meson.build says "no", which leaves t_vulkan out
#    of build.ninja with nothing reporting anything.
ms_nvk_libs=$( { sed -n '/^nvk_whole_libs  *= *\[/,/^\]/p' meson.build
                 sed -n '/^nvk_test_libs  *= *\[/,/^\]/p' meson.build; } |
               grep -o "'[^']*\.a'" | tokens)
sh_nvk_libs=$(sed -n "/^HORIZON_NVK_TEST_LIBS=\"/,/\"\$/p" \
              scripts/toolchain-env.sh |
              sed 's/^HORIZON_NVK_TEST_LIBS="//; s/"$//' | tokens)
compare "NVK archive paths" "meson.build:$ms_nvk_libs" \
        "scripts/toolchain-env.sh:$sh_nvk_libs"

# 6b. The standalone suites, which two build systems list separately.
#     This is the comparison that was missing when t_fault, t_pbsize and
#     t_display went into meson.build and not into the Makefile: the
#     Makefile is a supported path — the one whose output was verified
#     on hardware first — and scripts/build-switch.sh silently built
#     three fewer .nro than the Meson path. Found by a reviewer's eye in
#     PR #7, which is exactly the job a gate is supposed to do instead.
#
#     Only the suites that need nothing but the toolchain: the Mesa one
#     is comparison 1 and the NVK ones exist on the Meson path alone,
#     which is a deliberate asymmetry rather than a divergence.
mk_horizon=$(make_var SUITES | tokens)
ms_horizon=$(meson_suite_names horizon_suites | tokens)
compare "standalone suite names" "Makefile:$mk_horizon" \
        "meson.build:$ms_horizon"

# 6c. AND THE CASES INSIDE THEM, which is the comparison the split into
#     suites made necessary. A .nro that both build systems agree exists
#     is not the same .nro if one of them leaves a case out of it, and
#     nothing about the build would say so: the suite still links, still
#     runs, and reports a smaller number of cases that nobody is
#     comparing against anything.
#
#     Every suite both paths build, which is the standalone ones plus
#     the Mesa one. The NVK suites are meson.build's alone.
for s in $mk_horizon $mk_tests; do
    mk_cases=$(make_var "CASES_$s" | tokens)
    ms_cases=$(meson_suite_cases horizon_suites "$s" | tokens)
    if [ -z "$(printf '%s' "$ms_cases" | tr -d ' ')" ]; then
        ms_cases=$(meson_suite_cases mesa_suites "$s" | tokens)
    fi
    compare "cases in $s" "Makefile:$mk_cases" "meson.build:$ms_cases"

    # And that each of them is a file. A case listed in both build
    # systems and written in neither fails at link time, which is the
    # right outcome but a long way from here; a case whose file exists
    # under a different name is the one this catches.
    for c in $mk_cases; do
        if [ ! -f "tests/$s/$c.c" ]; then
            echo "MESA TEST PARITY (cases in $s): tests/$s/$c.c does not"
            echo "  exist, but both build systems name that case."
            fail=1
        fi
    done
    if [ ! -f "tests/$s/suite.c" ]; then
        echo "MESA TEST PARITY (cases in $s): tests/$s/suite.c does not"
        echo "  exist; a suite without a table defines no cases at all."
        fail=1
    fi
done

# 7. Which shaders each NVK case compiles in. Two files state it:
#    meson.build's nvk_case_shaders decides which headers are assembled
#    and added to that suite's sources, and the case's own #include lines
#    decide which it uses.
#
#    Neither direction of a disagreement is caught by building. All the
#    generated headers land in one build directory, so a case that
#    includes a header meson did not give it still compiles — as long as
#    some *other* case in the same build asked for that shader. And a
#    case that lists a shader it does not include just assembles it for
#    nothing, which is invisible.
#
#    PER CASE AND NOT PER SUITE, although the build only needs the union.
#    That matters for vk_core/image_clear, which must reach its first
#    render pass with nothing uploaded to NVK's shader heap or it stops
#    being the regression test for patch 0030 (see its header comment).
#    "This case includes no shader" is a property of the case, and
#    comparing the union would let a shader one of its neighbours needs
#    answer for it.
ms_nvk_suites=$(meson_suite_names nvk_suites)
if [ -z "$(printf '%s' "$ms_nvk_suites" | tr -d ' ')" ]; then
    echo "MESA TEST PARITY (case shaders): extracted no NVK suites from"
    echo "  meson.build. The extraction in this script no longer matches"
    echo "  that file. Fix the extraction — an empty set must never be"
    echo "  reported as agreement."
    fail=1
fi

# The dict is read once; a per-case sed over the whole file would be the
# same answer thirty times over. Comments go first so a '# ...quoted...'
# inside an entry cannot be read as a shader name.
nvk_shader_dict=$(sed -n '/^nvk_case_shaders  *= *{/,/^}/p' meson.build |
                  sed 's/#.*//')

for suite in $ms_nvk_suites; do
    if [ ! -f "tests/$suite/suite.c" ]; then
        echo "MESA TEST PARITY (case shaders): tests/$suite/suite.c does"
        echo "  not exist, but meson.build names that suite."
        fail=1
        continue
    fi
    for c in $(meson_suite_cases nvk_suites "$suite"); do
        src="tests/$suite/$c.c"
        if [ ! -f "$src" ]; then
            echo "MESA TEST PARITY (case shaders): $src does not exist, but"
            echo "  meson.build names that case."
            fail=1
            continue
        fi
        # An entry may span lines, so it is accumulated from the key to
        # the closing bracket and the key itself dropped before the names
        # are read out. "(none)" rather than the empty string: compare()
        # treats empty as a broken extraction, and "this case uses no
        # shader" is a real value here — in fact the one worth guarding.
        ms_sh=$(printf '%s\n' "$nvk_shader_dict" |
                awk -v key="'$suite/$c'" '
                    $0 ~ "^ *" key " *:" { inside = 1 }
                    inside {
                        buf = buf $0
                        if (index($0, "]")) {
                            sub(/^[^[]*\[/, "", buf)
                            print buf
                            exit
                        }
                    }' |
                grep -o "'[^']*'" | tr -d "'" | LC_ALL=C sort -u | tr '\n' ' ')
        src_sh=$(grep -o '^#include "[A-Za-z0-9_]*\.spv\.h"' "$src" |
                 sed 's/^#include "\(.*\)\.spv\.h"$/\1/' |
                 LC_ALL=C sort -u | tr '\n' ' ')
        compare "shaders for $suite/$c" \
                "meson.build:${ms_sh:-(none)}" "$src:${src_sh:-(none)}"
    done
done

if [ "$fail" -eq 0 ]; then
    echo "check-mesa-test-parity: OK (mesa suite, archives, defines," \
         "includes, default build dir, NVK archives, standalone suites," \
         "cases in each, case shaders)"
fi
exit "$fail"
