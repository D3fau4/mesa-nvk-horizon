# devkitA64 gcc 15.2.0: `-mtp=soft -fPIC` miscompiles thread-local access

**Decision D7.** This is the report, written out so that filing it is a
copy-and-paste rather than a re-investigation. It has not been filed:
this container cannot reach devkitPro's tracker, and the finding belongs
to whoever posts it. Everything below was measured in this tree on
2026-07-28 and is reproducible from four lines.

Where to file: <https://github.com/devkitPro/buildscripts/issues>, or
the devkitPro Discord's toolchain channel if the maintainers prefer
that.

---

## Summary

On devkitA64 gcc 15.2.0, compiling with `-mtp=soft -fPIC` generates
wrong code for every access to a `_Thread_local` object. The compile
succeeds, the link succeeds, and the program reads and writes a wild
address the first time a thread-local is touched.

`-mtp=soft -fPIE` is correct. Only the `-fPIC` combination is affected.

## Reproducer

```c
/* tls.c */
_Thread_local int x;
int get(void) { return x; }
void set(int v) { x = v; }
```

```sh
$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc -O2 -mtp=soft -fPIE -c tls.c -o good.o
$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc -O2 -mtp=soft -fPIC -c tls.c -o bad.o
$DEVKITPRO/devkitA64/bin/aarch64-none-elf-objdump -dr good.o
$DEVKITPRO/devkitA64/bin/aarch64-none-elf-objdump -dr bad.o
```

## What the two produce

```
-mtp=soft -fPIE   bl  __aarch64_read_tp
                  add x0, x0, #0x0, lsl #12   R_AARCH64_TLSLE_ADD_TPREL_HI12
                  add x0, x0, #0x0            R_AARCH64_TLSLE_ADD_TPREL_LO12_NC

-mtp=soft -fPIC   bl  __aarch64_read_tp
                  lsl x0, x0, #1              (no relocation emitted)
```

The correct form adds the variable's link-time offset to the thread
pointer in two halves, each with a relocation for the linker to fill in.
The `-fPIC` form **shifts the thread pointer left by one** — doubling it
— and emits no relocation at all, so there is nothing for the linker to
correct. Every subsequent read or write through that pointer lands at an
address unrelated to the variable.

## Why it is worth fixing rather than documenting

It is silent in every way a toolchain bug can be silent. No warning, no
error, no undefined symbol, no missing relocation reported by the
linker. The first symptom is a hang or memory corruption at run time, at
a point unrelated to the code that was miscompiled.

And it is easy to hit without meaning to. Meson appends `-fPIC` to
static-library objects unless `b_staticpic=false` is set, *after* the
cross file's `-fPIE`. Any Meson project that builds static libraries for
Horizon and uses thread-local storage gets the broken form by default.
In this tree it affected all three objects in a Mesa build that use TLS
and hung a threading test on its first run.

## Suggested fix, from a user rather than a toolchain maintainer

The soft thread-pointer path appears to lose the TLS relocation when the
addressing mode is chosen for `-fPIC`. Whatever the cause, the safe
short-term behaviour would be to reject the combination — an error on
`-mtp=soft -fPIC` is far better than code that runs and is wrong.

## How this project works around it

Two ways, both kept:

1. `-Db_staticpic=false` on every Meson build, so `-fPIC` is never
   appended after the cross file's `-fPIE`.
2. `scripts/check-tls-relocs.sh`, a gate over every object *and archive
   member* in the build: an object that calls `__aarch64_read_tp` and
   carries no `R_AARCH64_TLS*` relocation is the broken form and fails
   the build. It tests the property rather than the flag, so it keeps
   working if a future toolchain gets this wrong a different way.

The gate is what would catch a regression if the bug were fixed and came
back, and it has been deliberately broken to confirm it fails — see
`docs/history/phase-4-continued.md`.

---

Copyright (c) mesa-nvk-horizon contributors
SPDX-License-Identifier: MIT
