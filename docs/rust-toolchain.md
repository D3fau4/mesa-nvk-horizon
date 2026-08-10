# Rust toolchain for NAK/NIL on Horizon — answer to R13

**Status:** investigated and answered at the start of Phase 2, before any
toolchain was built, which is what `docs/known-risks.md` R13 asked for.

**Evidence base:** the Mesa tree at the commit pinned when this was written —
`mesa-26.1.5`, `6a02618ccf6c5651ecb9cccbde571eb61fd73592` (decision D2, pinned in
`toolchain/versions.env`). Every file:line below is from that checkout;
re-check with `scripts/fetch-mesa.sh` and the greps quoted in each section.

The pin moved to `mesa-26.1.6` (`ffa422e53d59a4938b38abd5c3fc319555da31dd`) on
2026-08-10 and **nothing quoted here moved with it**: the 26.1.5 → 26.1.6 diff
touches 114 files and not one of them is under `src/nouveau/compiler/` or
`src/compiler/rust/`, so the file:line references below are still exact rather
than approximate. Checked with
`git -C mesa diff --name-only mesa-26.1.5^{} mesa-26.1.6^{}`.

---

## 1. The question

R13: NVK needs NAK (the shader compiler) and NIL (the image-layout
library), both written in Rust. Building Rust `std` for a custom Horizon
target is described by the reference port as the single most fragile part
of the toolchain.

> **Open question:** whether `std` is actually required, or whether
> `no_std` + `alloc` suffices for NAK/NIL as Mesa links them. Answering
> this could remove the sysroot build entirely.

---

## 2. Answer

**As Mesa links them today, `std` is required.** There is no `#![no_std]`
anywhere in Mesa's `src/`, and the crates are built as Rust *staticlibs*,
which bundle the target's Rust runtime — including `libstd` — into the
archive:

```
$ grep -rn 'no_std' --include='*.rs' src/
(no matches)

src/nouveau/compiler/meson.build:106   _libnak_rs  … rust_abi : 'c'
src/nouveau/nil/meson.build:82         _libnil_rs  … rust_abi : 'c'
```

Meson's `rust_abi : 'c'` on a `static_library` means `--crate-type
staticlib`.

**But the dependency is shallow.** Almost everything NAK and NIL use from
`std` is a re-export of `core` (`cmp`, `ops`, `mem`, `ptr`, `slice`,
`marker`, `iter`, `hash`, `fmt`, `num`, `cell`, `array`) or of `alloc`
(`Box`, `Vec`, `String`, `BinaryHeap`, `BTreeSet`, `CString`). The parts
that genuinely need an operating system are these, and only these:

| Site | What it needs | Replacement |
|---|---|---|
| `src/nouveau/compiler/nak/api.rs:14,39` | `std::env::var`, to read the `NAK_DEBUG` flags | Mesa's own `os_get_option()` from `util/`, already reachable through the bindgen bindings |
| `src/nouveau/compiler/nak/api.rs:19,95` | `std::sync::OnceLock` for the `DEBUG` static | `core::sync::atomic` one-shot init |
| `src/nouveau/compiler/nak/api.rs:18,507` | `std::panic::catch_unwind` around the compile entry point | Inert under `panic = "abort"`, which is what the Horizon target already sets — the call cannot catch anything there |
| `src/nouveau/nil/image.rs:13,197,215` | `std::panic::catch_unwind`, same shape | Same |
| `src/nouveau/compiler/nak/union_find.rs:4,28` | `std::collections::HashMap` | `rustc_hash::FxHashMap`, which the crate *already* uses in ~10 other files (`const_tracker.rs`, `opt_bar_prop.rs`, `opt_lop.rs`, `opt_prmt.rs`, `reg_tracker.rs`, `spill_values.rs`, `sm50.rs`, …) |
| `src/nouveau/compiler/nak/opt_instr_sched_common.rs:431-439` | `std::fs`, `std::io` in `save_graphviz` | Debug helper, marked `#[allow(dead_code)]` and never called — `cfg`-gate it |
| `src/compiler/rust/memstream.rs:4`, `nir_instr_printer.rs:4` | `std::io::Result` as the error type over C's `u_memstream` | A local error type; no actual I/O happens in Rust |

Everything else that looked alarming is test-only. `hw_runner.rs`,
`hw_tests.rs` and `nvdisasm_tests.rs` — the files using `std::process`,
`std::fs`, `std::env` and `Mutex` — are gated behind `#[cfg(test)]` in
`src/nouveau/compiler/nak/lib.rs:44-50` and never reach the driver.

One nuance worth stating precisely: `FxHashMap` is not itself `no_std`.
In rustc-hash 2.x it is a type alias over `std::collections::HashMap`
with a different hasher, so the substitution above removes a *direct*
`std` use but does not by itself remove the `std` dependency — the
hashbrown-backed configuration of rustc-hash does. Mesa already vendors
that backend: `subprojects/hashbrown-0.14-rs.wrap` sits next to
`subprojects/rustc-hash-2-rs.wrap` (rustc-hash 2.1.1, pinned by sha256).

### Consequence

**Phase 2 builds no Rust `std` sysroot.** R13 goes from "the most fragile
part of the toolchain" to a small, mechanical, plausibly upstreamable
patch set. That work belongs in `mesa-patches/` in Phase 3/4, not here —
Phase 2's job was to find out, before building anything, whether the
sysroot was needed. It is not.

### Confirmed against the toolchain

The analysis above is source-level, but the conclusion was afterwards
checked against the real thing. Configuring the pinned Mesa tree with
`toolchain/horizon-aarch64.cross` reaches Mesa's
`add_languages('rust')` and stops there:

```
$ rustc --target aarch64-nintendo-switch-freestanding \
        -C linker=aarch64-none-elf-gcc s.rs
error[E0463]: can't find crate for `std`
  = note: the `aarch64-nintendo-switch-freestanding` target may not be installed
  = help: consider building the standard library from source with `cargo build -Zbuild-std`
```

That is exactly the predicted shape: the target is a real, built-in,
tier-3 target; what it lacks is a prebuilt `std`, and rustc itself
points at `-Zbuild-std` as the way out. It confirms both halves of the
answer — Mesa does ask for Rust unconditionally, and `std` is the thing
that is missing.

Still true, and worth keeping straight: **no Rust has been successfully
compiled for Horizon.** The step above is a failure reproduced on
purpose, not a build.

---

## 3. The Rust target: no custom target JSON

`docs/milestones.md` Phase 2 item 3 provisionally called for
`toolchain/aarch64-horizon.json`, and item 4 for a `rustc` wrapper "if
still required". Neither is required.

rustc already ships the target:

```
$ rustc --print target-list | grep switch
aarch64-nintendo-switch-freestanding
```

Its spec (tier 3) is exactly what this project needs:

| Field | Value | Why it matters |
|---|---|---|
| `os` | `horizon` | `cfg(target_os = "horizon")` works without a custom target |
| `metadata.std` | `false` | Confirms the no_std framing above is the supported one |
| `panic-strategy` | `abort` | Which is why `catch_unwind` is inert on this target |
| `features` | `+v8a,+neon,+crypto,+crc` | Matches devkitA64's `-march=armv8-a+crc+crypto` |
| `position-independent-executables` | `true` | Matches the `-fPIE` the Makefile and cross file use |
| `linker` | `rust-lld` (with an NRO-shaped link script) | Only relevant when rustc links the final binary; here it produces an archive for devkitA64's gcc to link |

The pinned image already carries a nightly with the `rust-src` component
(`rustup component add rust-src` in the image's build), so
`-Zbuild-std=core,alloc` is available without any extra setup — tier-3
targets have no prebuilt `core` to download.

`toolchain/aarch64-horizon.json` is therefore committed as a **snapshot
for drift detection, not as a target to build against**. Tier-3 specs
change between nightlies, and a silent change to the panic strategy,
feature set or linker flavour would surface much later as a codegen or
link mystery. `scripts/check-rust-target.sh` diffs the built-in spec
against the snapshot; `--update` re-snapshots after a deliberate re-pin.

---

## 4. Open sub-risk: `-mtp=soft`

devkitA64 compiles C with `-mtp=soft`, meaning the thread pointer is
obtained through a function call rather than by reading `TPIDR_EL0`
directly. The Rust target spec has no way to express this, and rustc will
emit the hardware register access if it emits TLS at all.

Under `no_std + alloc` there should be no thread-local storage in NAK or
NIL, so the question should not arise — but "should not" is not
"measured". This has to be checked when Rust code is first actually built
and linked for Horizon (Phase 3/4), by confirming the generated archives
reference no TLS relocations. Recorded here so it is not discovered by a
crash on console.

A second, smaller one: `alloc` needs a global allocator. On this target
that is a short shim over the allocator devkitA64's newlib already
provides, but it is a thing that must exist and does not today.

---

## 5. What Phase 2 therefore ships

- No Rust sysroot, no `rustc` wrapper, no custom target.
- `RUST_BUILD_STD=no` recorded in `toolchain/versions.env`. The Rust
  version itself is **not** recorded or pinned: the image installs
  rustup's rolling `nightly`, and like libnx the toolchain belongs to
  the environment rather than to this repository (R15). Which nightly a
  given build used is recorded in that build's artefact manifest.
- `toolchain/aarch64-horizon.json` + `scripts/check-rust-target.sh` as a
  drift tripwire.
- This document, so Phase 3 starts from an answer rather than from the
  open question.

---

## 6. Update from Phase 4 step 3 (2026-07-28) — measured, not predicted

Rust has now been compiled and linked for Horizon. Everything in this
section is a **cross build (X)**; the full write-up, with commands, is
in `STATUS.md` under "Phase 4 — step 3".

### § 2's conclusion is confirmed, and by a second route

The document concluded `no_std` + `alloc` from the source. A
measurement now says the same thing from the target's own metadata:

```
$ rustc --print cfg --target aarch64-nintendo-switch-freestanding
target_os="horizon"   target_env=""   (no target_family, no unix)

$ rustc --print cfg --target armv6k-nintendo-3ds
target_os="horizon"   target_env="newlib"   target_family="unix"   unix
```

Rust's `std` **does** support `target_os = "horizon"` — and every one of
those sites is under `sys/pal/unix/`, `sys/fs/unix.rs`,
`sys/alloc/unix.rs`. That is the **3DS**, which reaches newlib through
the unix PAL. The Switch target has no `target_family`, so a `std` built
for it selects `sys/pal/unsupported`, where the operations that make
`std` worth having return errors.

So building `std` is not the cheaper option that was passed over; it is
the worse one. `"std": false` in the spec is describing this.

### § 4's open sub-risk is now closed for the probe, open for NAK/NIL

The `-mtp=soft` question — "should not arise, but 'should not' is not
'measured'" — was checked on the artefact, as this section asked:

```
R_AARCH64_TLS* relocations in the Rust staticlib: 0
__aarch64_read_tp references:                     0
mrs tpidr_el0 in the linked Horizon ELF:          0
```

A `no_std` + `alloc` staticlib generates no thread-local storage, so
rustc never reaches the point where it would emit the hardware
thread-pointer read. **This was measured on a probe crate, not on NAK
and NIL**, and the check to re-run on their archives is the same one:
`scripts/check-tls-relocs.sh`, which tests the property rather than the
flag and will need pointing at the Rust output.

The second sub-risk — `alloc` needs a global allocator, "a thing that
must exist and does not today" — was satisfied in the probe by a
`#[global_allocator]` over newlib's `memalign`/`free`, six lines. What
it turned into is a **design question rather than a shim**: exactly one
`#[global_allocator]` and one `#[panic_handler]` may exist across the
whole crate graph, and Mesa links two Rust staticlibs into one binary.
Where they live is not decided here.

### What the sysroot turned out to be

`-Zbuild-std=core,alloc` produces three rlibs — `core`, `alloc`,
`compiler_builtins` — and installing them at
`lib/rustlib/<target>/lib/` is enough for a bare `rustc --sysroot` to
find them. That matters because **Meson drives `rustc` directly and
never calls cargo**, so cargo is a build-time tool for the sysroot only.

One condition of the environment had to be handled first: containers
here have no network, and `-Zbuild-std` resolves the standard library's
whole workspace, which depends on 30 crates.io packages even when only
`core` and `alloc` are built. `scripts/fetch-rust-crates.sh` fetches
them on the host against the checksums in Rust's own
`library/Cargo.lock`. It pins nothing itself, for the same reason
`versions.env` does not pin libnx.

---

## 7. Update from Phase 4 step 5 (2026-07-28) — the conversion, done

Everything in this section is a **cross build (X)**. The full write-up
is in `STATUS.md` under "Phase 4 — step 5".

**§ 2's seven sites were right, and all seven are gone.** What § 2 did
not count was the prelude: without `std` there is no `Vec`, `Box`,
`String`, `vec!` or `format!` in scope anywhere, which is why the first
build produced five hundred errors that were one fact. Measured before
starting: 37 files needing `use alloc::…`, ~210 `std::` paths of which
the overwhelming majority are `core::` under another name.

Two of § 2's specifics were wrong in detail, and both were found by
building rather than by reading:

- **`os_get_option()` was not "already reachable through the bindgen
  bindings".** NAK's allowlist covers `nak_.*`, `nouveau_ws_.*` and
  `drm.*`; it had to be added.
- **`FxHashMap` is not a substitution that removes `std`.** § 2 said as
  much in its own nuance paragraph, but the resolution is not the one
  it guessed: rustc-hash 2.x defines the aliases *only* under its
  `std` feature, and Mesa's wrap turned that feature on. The aliases
  are now rebuilt over hashbrown directly.

Four things were not on the list at all: `eprintln!` (21 sites),
`f32::round`/`powf`/`log2` (which live on `std`'s float types because
`core` has no libm), rustc-hash's feature, and bindgen's
`--use-core`.

### § 4's sub-risk is now closed on NAK and NIL

Step 3 could only measure the TLS question on a probe crate. On the
real artefact:

```
R_AARCH64_TLS* relocations in libnouveau_rust_runtime.a : 0
__aarch64_read_tp references                            : 0
```

### The allocator question, answered by a link failure

§ 6 left open "where the single `#[global_allocator]` and
`#[panic_handler]` live". It is not a preference. Two `no_std` Rust
staticlibs, each with its own, **cannot be linked into one binary**:

```
ld: libb.a(...): multiple definition of `__rustc::__rust_alloc';
    liba.a(...): first defined here
    ... and __rust_dealloc, __rust_realloc, __rust_alloc_zeroed,
        rust_begin_unwind
```

Measured with and without `-O`. Upstream Mesa gets away with two Rust
staticlibs because `std` supplies the shim and the archive member
holding it is simply not pulled the second time.

So NAK and NIL are rlibs where there is no `std`, and
`src/nouveau/rust_runtime` is the one staticlib that carries the pair.
Verified on the artefact: exactly one definition of each of
`__rust_alloc`, `__rust_alloc_zeroed`, `__rust_alloc_error_handler` and
`rust_begin_unwind`.

### Still unmeasured

Whether `compiler_builtins` collides with newlib's `memcpy` family at
link time. Nothing has linked the full driver yet — the build now stops
on `src/nouveau/winsys`, which is what `nvkmd_horizon` replaces.
