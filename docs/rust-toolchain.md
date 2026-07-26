# Rust toolchain for NAK/NIL on Horizon — answer to R13

**Status:** investigated and answered at the start of Phase 2, before any
toolchain was built, which is what `docs/known-risks.md` R13 asked for.

**Evidence base:** the Mesa tree at the pinned commit — `mesa-26.1.5`,
`6a02618ccf6c5651ecb9cccbde571eb61fd73592` (decision D2, pinned in
`toolchain/versions.env`). Every file:line below is from that checkout;
re-check with `scripts/fetch-mesa.sh` and the greps quoted in each section.

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

This is a source-level conclusion. Nothing Rust has been *compiled* for
Horizon yet, and this document does not claim otherwise.

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
