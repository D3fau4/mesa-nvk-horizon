#!/usr/bin/env bash
# Installs the NVK driver into a devkitPro portlibs prefix, and removes
# it again.
#
#   scripts/install-horizon.sh                      # build and install
#   scripts/install-horizon.sh --tarball <file>     # install a given one
#   scripts/install-horizon.sh --uninstall          # remove it
#
#   --prefix <dir>   where to install   default $DEVKITPRO/portlibs/switch
#   --destdir <dir>  staging root, prepended to the prefix and to
#                    nothing else
#
# $PREFIX and $DESTDIR are read too, and the options win.
#
# `make install` and `make uninstall` are these three lines.
#
# IT INSTALLS BY EXTRACTING THE TARBALL scripts/package-portlibs.sh
# builds, rather than by copying files itself. That is the whole design:
# CI publishes that tarball and this consumes it, so what a release
# contains and what an install places cannot drift apart — there is one
# procedure and one file set. It also makes two awkward things free.
# Idempotence: the tarball is byte-reproducible, so "is this install
# already current" is one file comparison. And installing a downloaded
# release is the same code path as installing a local build, with
# --tarball naming the file.
#
# THE DEFAULT PREFIX IS NOT A CONVENIENCE. devkitPro's
# aarch64-none-elf-pkg-config clears PKG_CONFIG_PATH and sets
# PKG_CONFIG_LIBDIR to $DEVKITPRO/portlibs/switch/lib/pkgconfig and
# nothing else, so that is the one directory in which a .pc is found at
# all. Installing anywhere else means passing --with-path by hand.
#
# Copyright (c) mesa-nvk-horizon contributors
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."

# Relative to $PREFIX. scripts/package-portlibs.sh writes both into the
# tarball; this file only reads them.
MANIFEST_REL=share/mesa-nvk-horizon/installed-files.txt
RECORD_REL=share/mesa-nvk-horizon/INSTALL.txt

MODE=install
TARBALL=""
# --prefix and --destdir override $PREFIX and $DESTDIR, and the Makefile
# uses them rather than the environment. That is not a style preference:
# devkitPro's make on Windows is a Cygwin build, and an environment
# assignment in front of a recipe command reaches NO child of it —
# measured, `FOO=hello sh -c 'echo $FOO'` from a recipe prints nothing.
# So `make install PREFIX=...` would have installed into the default
# prefix while printing the one it was asked for. Arguments cross that
# boundary; exported variables do not.
while [ "$#" -gt 0 ]; do
    case "$1" in
        --uninstall) MODE=uninstall ;;
        --tarball)   shift; TARBALL="${1:?--tarball needs a file}" ;;
        --prefix)    shift; PREFIX="${1:?--prefix needs a directory}" ;;
        # May legitimately be empty, so it is not checked for emptiness.
        --destdir)   shift; DESTDIR="${1-}" ;;
        -h|--help)
            sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "error: unknown argument '$1'" >&2
            exit 1 ;;
    esac
    shift
done

# $PREFIX BEFORE ANYTHING ELSE, AND WITHOUT scripts/toolchain-env.sh.
#
# Sourcing that file is what puts an uninstall out of reach exactly when
# it is wanted: with no $DEVKITPRO and no docker it exits outright, so
# somebody trying to delete 68 files would be told to start a daemon.
# Removing an install also has to work on a tree whose build has been
# wiped, whose mesa/ checkout is gone, or which is mid-rebuild — states
# in which every gate below would fire. So uninstall needs $PREFIX and
# nothing else, and this is where it gets it.
if [ -z "${PREFIX:-}" ]; then
    if [ -z "${DEVKITPRO:-}" ]; then
        echo "error: neither \$PREFIX nor \$DEVKITPRO is set, so there is" \
             "no way to tell where the install is." >&2
        echo "       Set \$DEVKITPRO to a devkitPro installation, or" \
             "\$PREFIX to a portlibs prefix directly." >&2
        exit 1
    fi
    PREFIX="$DEVKITPRO/portlibs/switch"

    # $DEVKITPRO IS A PATH AS THE COMPILER SEES IT, WHICH IS NOT ALWAYS
    # A PATH THIS SHELL CAN REACH — and the failure is not a refusal, it
    # is a success in the wrong place. Under Git Bash on Windows a
    # $DEVKITPRO of /opt/devkitpro resolves inside the Git installation
    # (cygpath -w /opt says so), where mkdir -p succeeds and nothing
    # will ever look. Hundreds of MiB installed, exit status 0.
    #
    # devkitA64/ is the probe because it is what makes a directory a
    # devkitPro rather than an empty path that happened to be
    # creatable.
    if [ ! -d "$DEVKITPRO/devkitA64" ]; then
        echo "error: \$DEVKITPRO is $DEVKITPRO but this shell finds no" \
             "devkitA64 there, so $PREFIX would not be the prefix the" \
             "compiler uses." >&2
        echo "       On Windows, run this from devkitPro's own MSYS2" \
             "shell, where /opt/devkitpro is mapped — or set \$DEVKITPRO" \
             "to the path this shell resolves." >&2
        exit 1
    fi
fi
DESTDIR="${DESTDIR:-}"
ROOT="$DESTDIR$PREFIX"

# ---------------------------------------------------------------------
# UNINSTALL
# ---------------------------------------------------------------------
if [ "$MODE" = uninstall ]; then
    if [ ! -f "$ROOT/$MANIFEST_REL" ]; then
        echo "error: no $ROOT/$MANIFEST_REL, so nothing here records what" \
             "was installed." >&2
        echo "       Refusing to guess: the file set depends on the build" \
             "that produced it, and guessing means rm -f inside" \
             "\$DEVKITPRO from a build that may no longer exist." >&2
        exit 1
    fi

    # A COPY FIRST, BECAUSE THE MANIFEST LISTS ITSELF. It has to — it is
    # a file the install placed, and anything not in it is never removed
    # — but that means the loop below deletes the file the loop after it
    # reads. Measured: the files went, awk then failed to open the
    # manifest, and every directory this created was left behind empty.
    UWORK="build/uninstall-tmp.$$"
    trap 'rm -rf "$UWORK"' EXIT INT TERM
    mkdir -p "$UWORK"
    cp "$ROOT/$MANIFEST_REL" "$UWORK/manifest"

    _removed=0
    _absent=0
    while IFS= read -r _f; do
        [ -n "$_f" ] || continue
        if [ -e "$ROOT/$_f" ]; then
            rm -f "$ROOT/$_f"
            _removed=$((_removed + 1))
        else
            _absent=$((_absent + 1))
        fi
    done < "$UWORK/manifest"

    # rmdir, NEVER rm -r, deepest first, and only directories the
    # manifest's own paths pass through.
    #
    # rmdir refusing a non-empty directory IS the protection, and it is
    # better than any exclusion list this could carry: lib/, include/
    # and lib/pkgconfig/ belong to devkitPro and are full of devkitPro's
    # files, so they survive without being named, while lib/nvk/,
    # include/nvk/, include/horizon_gpu/, licenses/mesa-nvk-horizon/ and
    # share/mesa-nvk-horizon/ are ours and empty by now, so they go.
    # There is no way to write a rule here that deletes somebody else's
    # directory.
    awk -F/ '{ p = ""
               for (i = 1; i < NF; i++) {
                   p = (i == 1 ? $i : p "/" $i)
                   print i, p
               } }' "$UWORK/manifest" |
        # Unique on the whole line FIRST, then ordered by depth. `sort
        # -rn -u` in one pass does not do this: -u drops duplicates by
        # the comparison key, and the numeric key is only the leading
        # depth, so every directory at a given depth but one was thrown
        # away. Measured — lib/nvk, include/nvk and four others were
        # left behind empty.
        LC_ALL=C sort -u | LC_ALL=C sort -s -k1,1rn |
        while read -r _depth _dir; do
            [ -n "$_depth" ] || continue
            rmdir "$ROOT/$_dir" 2>/dev/null || true
        done

    echo "install-horizon: $ROOT — $_removed file(s) removed," \
         "$_absent already absent"
    exit 0
fi

# ---------------------------------------------------------------------
# INSTALL
# ---------------------------------------------------------------------
if [ -z "$TARBALL" ]; then
    # scripts/package-portlibs.sh carries every gate about whether the
    # build is complete and current, and is cheap on a second run: the
    # archives compare equal and the tarball is not rebuilt. Repeating
    # any of that here would be a second copy of it.
    scripts/package-portlibs.sh
    TARBALL=build/mesa-nvk-horizon-portlibs.tar.gz
fi
[ -f "$TARBALL" ] || {
    echo "error: no tarball at $TARBALL" >&2
    exit 1
}

WORK="build/install-tmp.$$"
trap 'rm -rf "$WORK"' EXIT INT TERM
mkdir -p "$WORK"

# The two files that describe the package, read out of the tarball
# rather than out of the staging directory: what is about to be
# extracted is what matters, and with --tarball there is no staging
# directory at all.
tar -xzOf "$TARBALL" "./$MANIFEST_REL" > "$WORK/manifest.new"
tar -xzOf "$TARBALL" "./$RECORD_REL"   > "$WORK/record.new"
[ -s "$WORK/manifest.new" ] || {
    echo "error: $TARBALL carries no $MANIFEST_REL, so an install from it" \
         "could not be undone. It was not made by" \
         "scripts/package-portlibs.sh." >&2
    exit 1
}

# ALREADY CURRENT? INSTALL.txt holds the sha256 of every file in the
# package except itself, so comparing that one file compares the whole
# install — and because the tarball is byte-reproducible, an unchanged
# build genuinely produces an identical one. This is what makes
# installing twice change nothing, rather than rewrite 225 MiB
# identically.
if [ -f "$ROOT/$RECORD_REL" ] && cmp -s "$WORK/record.new" "$ROOT/$RECORD_REL"; then
    echo "install-horizon: $ROOT is already current" \
         "($(wc -l < "$WORK/manifest.new" | tr -d ' ') files); nothing written"
    exit 0
fi

mkdir -p "$ROOT"

# ANYTHING THE LAST INSTALL PUT HERE THAT THIS ONE DOES NOT.
#
# Without this, an archive dropped from $HORIZON_NVK_TEST_LIBS or a
# header deleted upstream stays in the prefix forever, and the new
# manifest does not name it — so no later uninstall would remove it
# either, and every build on the machine goes on seeing it. It is
# package-horizon.sh's "dropping <name> — not in <src>" applied to a
# destination that outlives the build directory.
if [ -f "$ROOT/$MANIFEST_REL" ]; then
    LC_ALL=C sort "$ROOT/$MANIFEST_REL" > "$WORK/manifest.old"
    LC_ALL=C sort "$WORK/manifest.new" > "$WORK/manifest.new.sorted"
    _gone=$(LC_ALL=C comm -23 "$WORK/manifest.old" "$WORK/manifest.new.sorted")
    if [ -n "$_gone" ]; then
        echo "install-horizon: removing" \
             "$(printf '%s\n' "$_gone" | wc -l | tr -d ' ')" \
             "file(s) the previous install placed and this one does not"
        printf '%s\n' "$_gone" | while IFS= read -r _f; do
            [ -n "$_f" ] || continue
            rm -f "$ROOT/$_f"
        done
    fi
fi

tar -xzf "$TARBALL" -C "$ROOT"

# GATE, OVER WHAT LANDED RATHER THAN OVER WHAT WAS PACKAGED.
#
# scripts/package-portlibs.sh already refuses to build a tarball holding
# a thin archive. Asking again here is not the same question: this one
# is about the prefix, so it also covers a file an earlier install left
# and a tarball somebody made another way and passed with --tarball. It
# is the same reason package-horizon.sh reads build ids out of the .nro
# instead of out of the tree.
#
# Fatal, not a warning. A thin archive installed is a prefix that looks
# healthy and fails at the next project's link with "error opening thin
# archive member", a long way from here.
_thin=""
while IFS= read -r _f; do
    case "$_f" in *.a) ;; *) continue ;; esac
    [ -f "$ROOT/$_f" ] || continue
    [ "$(head -c 7 "$ROOT/$_f")" = '!<thin>' ] && _thin="$_thin $_f"
done < "$WORK/manifest.new"
if [ -n "$_thin" ]; then
    echo "error: these archives installed thin:$_thin" >&2
    echo "       They name objects they do not contain and will fail at" \
         "link time. Run scripts/install-horizon.sh --uninstall and" \
         "rebuild the package." >&2
    exit 1
fi

echo "install-horizon: $ROOT —" \
     "$(wc -l < "$WORK/manifest.new" | tr -d ' ') files from $TARBALL"
echo "install-horizon: consume it with" \
     "\`aarch64-none-elf-pkg-config --cflags --libs nvk\`"
