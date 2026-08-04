# Phase 4 — the running narrative that lived in the Current state block

Moved out of `STATUS.md` when D17 was closed. This was written a step at
a time while Phase 4 was in progress, in the block that is supposed to
say what is true *now*; it says what was true then, which is what
history is for. Not reworded — only moved.

---

**Phase 4 was:** `nvkmd_horizon`, the backend NVK talks to. All ten milestone
items implemented, the driver linked as a `.nro`, and the mandatory sequence
run on console.

**Step 1** is the interface reading: what `nvkmd` requires, operation by
operation, with its semantics, against what `horizon_gpu` already
provides. It exists to answer one question before any code: **is Phase 4
implementation, or also an extension of `horizon/`?** The answer is
*both*, and the extensions are six, small, and enumerated — four of them
conditional on Vulkan-side decisions that are named (D9–D12) and
deliberately not taken yet.

**Step 2** closed the six unresolved libc symbols, and the result is an
artefact rather than an argument: **an executable that pulls NIR,
SPIR-V, `nir_print`, `mesa_log` and `util_sparse_array` out of Mesa's
built core links, with zero undefined symbols.** Two new patches (0013,
0014 — the series is now **fourteen**), nothing added to `compat/`, and
the two symbols that remain unresolved in the archives are in members
nothing references, which the link demonstrates rather than assumes.

**Step 3** reproduced the Rust failure first
(`error[E0463]: can't find crate for 'std'`, at `mesa/meson.build:841`)
and then got past it: **Rust has been compiled and linked for Horizon
for the first time in this project** — a `no_std` + `alloc` staticlib,
linked by devkitA64 into a Horizon ELF with zero undefined symbols and
**zero TLS relocations**, which also measures the `-mtp=soft` sub-risk
`docs/rust-toolchain.md` § 4 left open. `scripts/fetch-rust-crates.sh`
is new: the container has no network and `-Zbuild-std` needs crates.io,
so the 30 packages are fetched on the host against Rust's own lockfile
checksums, and the check was broken four ways before being trusted.

**Phase 3 is closed except for one thing, unchanged:** `t_threads` and
`t_ostime` have never run on a console (they pass on Eden — 67/67 and
43/43). That does not block Phase 4 and is recorded as owed. The full
Phase 3 state is kept verbatim below under "Phase 3 — the state it
closed in".

All three known blockers carried into this phase are now addressed: the
libc symbols (step 2), Rust (step 3), and `-Db_staticpic=false`, which
remains mandatory with its gate running after every Mesa build.

**What is not written yet is `nvkmd_horizon` itself.** The nearest
obstacle is named and measured: NAK and NIL are still `std` crates, so
Mesa's Rust half does not build until the seven substitutions
`docs/rust-toolchain.md` § 2 lists become patches.

**Step 4** is the build machine. `meson setup -Dvulkan-drivers=nouveau`
now **configures and builds for Horizon**: the C half of the nouveau
Vulkan driver compiles, and bindgen generates NAK's, NIL's and
`compiler`'s bindings. Getting there needed four things the toolchain
did not have — libclang, bindgen, cbindgen and a Rust sysroot for a
tier-3 target — and then a fifth, `mesa_clc`, because NVK compiles two
OpenCL C files into SPIR-V at build time. On the owner's suggestion
these stopped being mounted directories and became a **derived Docker
image** (`toolchain/Dockerfile`), which is both tidier and the reason
`build/` is disposable again. Patch 0015 makes the driver a static
library where there is no dynamic loader; the series is fifteen.

**Where it stops, and why that is progress:** `rustc` fails with five
hundred errors that are one fact — with no `std` in the sysroot there
is no prelude. That is the `no_std` conversion arriving as a compiler
error instead of as a plan, and **D13 is now decided by measurement**:
two `no_std` Rust staticlibs cannot be linked into one binary
(`multiple definition of __rust_alloc`, five symbols), so NAK and NIL
become rlibs behind a single staticlib that carries the one
`#[global_allocator]` and `#[panic_handler]`.

**Step 5** built it. **`libnouveau_rust_runtime.a`, 15 292 744 bytes,
is Mesa's entire Rust half compiled `#![no_std]` for the Switch
target** — NAK, NIL, `compiler`, `nvidia_headers`, `bitview`,
`nak_latencies` and both bindgen crates — with **0 TLS relocations** and
**exactly one** definition of each allocator and panic symbol.
Reproduced byte-identically from a reset `mesa/` and a re-applied
series. Patches 0016 and 0017; the series is seventeen.

**What the build stops on now is the right thing:**
`src/nouveau/winsys` — libdrm talking to the nouveau kernel driver —
does not compile against newlib, and it is not supposed to. Replacing
it *is* `nvkmd_horizon`.

**`nvkmd_horizon` itself now exists.** `libnvk.a`, 94 162 bytes, is the
nouveau Vulkan driver compiled for Horizon against the `nv` system
services — `vk_icdGetInstanceProcAddr` and `nvk_CreateInstance` defined,
0 TLS relocations, built end to end from a clean tree by
`scripts/build-mesa-nvk.sh` in 4 m 17 s. **D9 and D10 are closed**, by
the hardware and by where the knowledge belongs respectively, and the
seven rejected designs are checked one by one in the section below.
**Items 3, 4 and 5 followed** — memory objects, the VA heap and
bind/unbind — so the driver can now allocate memory, reserve GPU address
space and map one into the other. Items 6 to 10 return
`VK_ERROR_FEATURE_NOT_PRESENT` with their item number, so every gap is
named. Patches 0018 and 0019; the series is nineteen.

**All ten items are implemented and `t_vulkan.nro` links** — 14 000 184
bytes, zero undefined symbols, zero multiple definitions. D11 is
resolved native over syncpoints and D8 where it actually bites: the
absolute deadline becomes a relative duration exactly once. The
`compiler_builtins`-versus-newlib question is answered by that link:
**no collision**. The series is twenty-three.
