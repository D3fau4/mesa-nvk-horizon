## Phase 2 — toolchain (2026-07-26)

No code in `horizon/` was touched. This phase is toolchain only.

### Environment facts (measured, not assumed)

| Resource | State |
|---|---|
| `pkg.devkitpro.org`, `apt.devkitpro.org` | **403** — devkitPro cannot be installed or queried over the network here |
| `gitlab.freedesktop.org` | reachable; `git ls-remote`, `git fetch` and `/-/archive/` tarballs all work |
| `github.com` raw / codeload | **403 / 404** |
| `pypi.org` | reachable |
| Containers (`--bridge=none`) | **no network**; anything fetched must be fetched on the host and mounted in |

### What is pinned, and what deliberately is not

**Owner's decision, taken during this phase: the Switch toolchain
belongs to the environment, not to this repository.** libnx, devkitA64
and the portlibs are neither pinned nor updated from here — they are
whatever `$DEVKITPRO` points at, or whatever is inside the container
image the developer runs. Updating libnx is `dkp-pacman -Syu` or a newer
image, not an edit to this tree. libnx moves fast and this backend is
written against exactly the `nv` services it exposes, so following it is
worth more than freezing it.

`toolchain/versions.env` is split accordingly:

| Half | Contents |
|---|---|
| ENVIRONMENT | How to *reach* the toolchain only: image repo/tag, the devkitPro prefix inside it, the `PATH` quirk, the target triple. **No version of anything.** |
| PINNED | What this project chooses: Mesa (`mesa-26.1.5` @ `6a02618ccf6c`), Meson `1.11.2`, the Rust target name. |

That line is deliberate: inputs this project *chooses* are pinned; the
environment it *runs in* is not. Mesa stays pinned because
`mesa-patches/` applies to a specific tree and a Mesa that moved
underneath would break Phase 3 silently.

**What replaces pinning is recording.** `scripts/package-horizon.sh`
writes into `build/pkg/MANIFEST.txt`, per build: the resolved image
digest, the live `dkp-pacman -Q` output, the rustc banner, each
artefact's sha256, and the exact
`HORIZON_NX_IMAGE=…@sha256:… scripts/build-horizon.sh` command that
rebuilds against the same toolchain. **This is what keeps a hardware
result attributable** when the inputs are not frozen.
`scripts/print-toolchain-versions.sh` is a read-only reporter feeding
it; it compares against nothing and updates nothing.

Observed in this environment at the time of writing (recorded as
evidence, **not** as a pin): devkitA64 `r29.2-1`, gcc `15.2.0-7`
(`aarch64-none-elf-gcc (devkitA64) 15.2.0`), binutils `2.45.1-2`
(`GNU ld 2.45.1`), newlib `4.6.0.20260123-4`, switch-tools `1.13.1-1`,
libnx `4.12.0-1`, deko3d `0.5.0-1`, `rustc 1.99.0-nightly (008fa22ce
2026-07-25)`, image digest `sha256:61a38fe4…`.

Why a libnx version number would have been misleading even if we had
pinned it — R15 in concrete form: the image installs the libnx package
and then builds `switchbrew/libnx` git HEAD over it.
`dkp-pacman -Qkk libnx` → **`226 total files, 205 altered files`**, with
`libnx.a` and `libnxd.a` mismatching on size, MD5 *and* SHA256. The
package version describes 21 of 226 files.
`print-toolchain-versions.sh` prints that measurement every time, so the
limitation is visible rather than assumed.

Also recorded because it bites: the image's own `PATH` does **not**
include `devkitA64/bin`. Anything resolving the cross compiler by bare
name — which the Meson cross file does deliberately — must prepend it.

### R13 answered — no Rust sysroot is built

Full evidence in `docs/rust-toolchain.md`, taken from the checked-out
tree at `MESA_COMMIT`. Summary: `std` **is** required as Mesa links
NAK/NIL today (no `#![no_std]` anywhere in `src/`; both built with
`rust_abi : 'c'` = `--crate-type staticlib`, which bundles libstd), but
the dependency is seven sites deep, all with direct replacements, and
the files using `std::process`/`fs`/`env` are `#[cfg(test)]`-gated and
never reach the driver. Closing the gap is a small `mesa-patches/` job
for Phase 3/4.

Milestone items 3–5 (custom target JSON, `rustc` wrapper, std sysroot)
are therefore **not needed**: rustc already ships
`aarch64-nintendo-switch-freestanding` (tier 3, `os = "horizon"`,
`std = false`, `panic = abort`, `+v8a,+neon,+crypto,+crc`, PIE) matching
devkitA64's flags. `toolchain/aarch64-horizon.json` is committed only as
a drift snapshot, checked by `scripts/check-rust-target.sh`.

This is a **source-level** conclusion. No Rust has been compiled for
Horizon yet.

### Meson cross file and build

`toolchain/horizon-aarch64.cross` is committed with **no absolute
paths**: bare `[binaries]` names resolved through `PATH`, and a
`devkitpro` constant it deliberately never defines.
`scripts/gen-cross-file.sh` writes that constant — and only that — into
a gitignored three-line file. Verified that Meson shares `[constants]`
across every `--cross-file` on one command line, so the two compose
without duplication. devkitPro's own generator
(`$DEVKITPRO/meson-toolchain.sh`) bakes in `which`-resolved absolute
paths, which is exactly what the gate forbids; its `[host_machine]`
block is matched exactly (`horizon`/`aarch64`/`cortex-a57`/`little`).

`meson.build` builds `libhorizon_gpu.a` and the ten `.nro`. The
`Makefile` is untouched and remains the reference path — it produced the
artefacts verified on console.

**Comparing the two paths found one real divergence.** Meson appends
`-fPIC` to static-library objects (`b_staticpic` defaults true),
overriding the cross file's `-fPIE` for those objects but not for the
executables' own. Measured on `t_alloc`: 48 bytes of `.text`, 32 of
`.bss`. Fixed with `b_staticpic=false`, plus a hard error in
`meson.build` if it is ever true — because Meson applies
`default_options` only on a build directory's *first* configure, so
reconfiguring an old directory would silently restore it.

After the fix the paths agree:

| Check | Result |
|---|---|
| `.nro` size, all ten tests | **identical 10/10** |
| Symbol sets (`nm`), `t_init` | identical (empty diff) |
| `.bss` symbols, `t_init` | identical count (149) and summed size (17183 bytes) |
| TLS region (`__tls_end - __tls_start`) | identical (`0x410`) |
| Residual | 32 bytes of `.bss` section padding, ≤16 of `.text` — inter-object padding from Meson's `--start-group`/archive ordering vs the Makefile's explicit order |

Also checked rather than assumed: Meson's automatic
`-D_FILE_OFFSET_BITS=64` is a **no-op** on devkitA64's newlib (`off_t`
already 8 bytes, `struct stat` `0x68` with and without), so it is left
alone.

### The cross file's "+ Mesa" half, validated

`docs/milestones.md` item 2 asks for a cross file for `horizon` **and
Mesa**. The Phase 1 half was exercised by building the ten `.nro`; the
Mesa half was, at first, `sys_root` and `pkg_config_libdir` written from
reasoning and never run. Configuring the pinned Mesa tree with it closed
that gap.

It works further than expected — Mesa accepts the machine description,
detects `aarch64-none-elf-gcc/g++ 15.2.0` for the host machine and gets
840 lines into its own `meson.build` — and it found one real defect **in
the cross file**, not in Mesa:

- Mesa calls `add_languages('rust')` unconditionally for the nouveau
  Vulkan driver and fails with *"'rust' compiler binary not defined in
  cross file [binaries] section"*. Fixed: `rust` and `bindgen` are now
  declared, and `horizon_run` puts the image's rustup on `PATH` (the
  image keeps it outside the default one).

With that, configuration proceeds to the Rust sanity check and stops
where R13 predicted:

```
error[E0463]: can't find crate for `std`
  = note: the `aarch64-nintendo-switch-freestanding` target may not be installed
  = help: consider building the standard library from source with `cargo build -Zbuild-std`
```

**This is R13 confirmed against the toolchain**, not just against the
source. It is a failure reproduced deliberately — no Rust has been
successfully compiled for Horizon.

Two further items handed to Phase 3, found here rather than later:

- `Checking for size of "void*" : -1`. **Corrected in Phase 3 (see
  "Phase 3 — item 3" below): this was recorded here as a consequence of
  `needs_exe_wrapper = true`, and that was wrong.** Meson's size check
  is a *compile*-time binary search, not an execution, so an exe wrapper
  has nothing to do with it. The real cause was this repository's own
  cross file putting `-Werror` in `c_args`, which Meson also passes to
  its detection snippets; the snippet tripped `-Werror=unused-variable`.
  With the warnings moved out, the same configure answers
  `void* : 8`. `needs_exe_wrapper = true` is unaffected and stays — it
  is correct, there is no emulator.
- `WARNING: cannot auto-detect -mtls-dialect when cross-compiling`.
  Directly adjacent to the open `-mtp=soft` sub-risk in R13.

The probe wrote nothing and patched nothing; the build directory was
deleted afterwards.

### How far Mesa's non-driver core gets — the Phase 3 starting line

Phase 3's exit criterion is "Mesa configures for `horizon` and builds
the non-driver core". Configuring with no drivers at all skips the Rust
check entirely (no nouveau driver, no `add_languages('rust')`), so that
criterion is reachable without touching R13. Probed with
`-Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dopengl=false
-Dllvm=disabled`:

1. **`Python (3.x) mako module >= 0.8.0 required to build mesa`** —
   milestone item 6 in concrete form. Mesa's generators are Python and
   the image ships no mako, no pyyaml, no pip and has no network.
   **Fixed here**, pinned in `versions.env` and installed on the host by
   `horizon_ensure_python_deps`.
2. With that, configure runs deep into real compile-and-link checks
   against the Horizon toolchain — `strtod` locale support, `Bsymbolic`,
   version scripts, `-Wl,--build-id=sha1` (milestone Phase 3 item 8,
   already answered: **supported**) — and stops at:

   ```
   Checking for function "dlopen" : NO
   mesa/meson.build:1684:16: ERROR: C shared or static library 'dl' not found
   ```

**That is where Phase 3 starts**: `libdl` does not exist on
newlib/libnx, which is Phase 3 item 3 ("newlib/libnx gaps"). Not a
toolchain problem — the toolchain is answering correctly.

Worth noting for Phase 3's plan: the milestone lists OS detection first,
but the configure order means the newlib/libnx gaps are what actually
block first. The list is a set of items to complete, not an order to
follow.

### Commands run and results

All on this machine. **Host build/run (H)** and **cross build (X)** only
— nothing in this phase needed a console.

| Command | Class | Result |
|---|---|---|
| `rm -rf build && scripts/configure-horizon.sh` | H | installs pinned meson 1.11.2, writes cross constants, configures; host compiler detected as `aarch64-none-elf-gcc (devkitA64) 15.2.0`; 32 build targets |
| `scripts/build-horizon.sh` | X | **10/10 `.nro`**, `-Wall -Wextra -Werror` clean, zero warnings |
| `scripts/package-horizon.sh` | H | 10 copied + `MANIFEST.txt` with sha256 per artefact and the toolchain pins |
| `scripts/build-switch.sh all -j4` (Makefile path) | X | exit 0, 10 `.nro`, no warnings (`-j4` did **not** hit the overlayfs race this time) |
| `scripts/check-no-abs-paths.sh` | H | **OK** (`toolchain scripts Makefile meson.build`) |
| `scripts/check-layering.sh` | H | OK |
| `scripts/check-rust-target.sh` | H | OK — built-in target matches the snapshot (reports drift, exits 0: the environment's nightly is not ours to pin) |
| `scripts/print-toolchain-versions.sh` | H | reports the live toolchain; feeds the artefact manifest |
| `scripts/run-host-tests.sh` | H | **103/103 PASS** (6 suites), no regression |
| `scripts/fetch-mesa.sh` | H | `mesa-26.1.5` checked out, **HEAD verified = `6a02618ccf6c…`**, 503 MB |
| Every script re-run a second time | H | idempotent: "already installed" / "unchanged" / "0 copied, 10 already current" / "nothing to do" |

### Phase 2 exit criteria

| Criterion (`docs/milestones.md`) | State |
|---|---|
| A clean container reproduces the toolchain from `scripts/` alone (H) | ✅ `build/` deleted entirely, then configure → build → package from scripts only. **Read as "reconstructs against the current environment", not "bit-identical forever"** — with the Switch toolchain unpinned by decision, a rebuild months later uses a newer libnx. `build/pkg/MANIFEST.txt` records which one, and the `HORIZON_NX_IMAGE=…@sha256:…` command to go back to it |
| `grep` for `/home/`, `/work`, `D:\`, `/mnt/` in `toolchain/` and `scripts/` returns nothing | ✅ gate green — and it **caught a real violation on its first run** (`scripts/build-switch.sh` mounted at a hardcoded `/work`; now mounts at `"$PWD"`) |
| Phase 1 tests cross-compile with the new cross file (X) | ✅ 10/10, identical in size to the Makefile's |

**Every Phase 2 exit criterion is met.**

### Deviations from the milestone item list, with reasons

| Item | Disposition |
|---|---|
| 1. devkitA64 / devkitPro pinned by package version | **Deliberately not done** — owner's decision during this phase. The Switch toolchain belongs to the environment; it is read and recorded per build, never pinned. R15's original mitigation is rejected rather than implemented, and R15 now says so. The `resolved from $DEVKITPRO` half of the item *is* done |
| 3. Rust target JSON for Horizon | **Not created as a target.** rustc ships `aarch64-nintendo-switch-freestanding`; the file is committed only as a drift snapshot (R13) |
| 4. `rustc` wrapper | **Not needed** — no custom target, so nothing to wrap |
| 5. Rust `std`/`core` sysroot, pinned nightly | **Not built** — R13's answer removes it. The nightly is the environment's, not pinned here |
| 6. Mesa host tools (native build for generators) | **Done**, once the probe below showed what the item concretely is: Mesa's generators are Python and need `mako` (and `pyyaml`), which the image ships neither of, along with no pip and no network. Pinned in `versions.env` and provisioned by `horizon_ensure_python_deps`. An earlier revision of this file recorded the item as deferred; that was wrong — it was a Phase 2 gap and it is now closed |

### Is Phase 2 finished?

Yes, for everything verifiable without a console. All three exit
criteria are met, and the one item that was genuinely outstanding
(milestone item 6) is closed above.

**One thing is owed and it is not Phase 2's:** the ten `.nro` have not
been re-run on hardware since Phase 1's second review round. Phase 2
changed no `horizon/` code, so nothing here made that worse — but the
longer it waits, the more work sits on an unreconfirmed base. It does
not block Phase 3, which touches Mesa and not `horizon/`; it does block
Phase 4, which builds directly on `horizon/`.

### Known gaps at the end of Phase 2

- Nothing Rust has been **compiled** for Horizon. R13's answer is
  source-level, and the `-mtp=soft` sub-risk stays open until it is.
- `mesa/` is fetched but **never configured or built**. That is Phase 3.
- The `.nro` produced by the Meson path are a **cross build**, not
  hardware-verified. They are identical in size to the Makefile's, whose
  hardware run is itself still pending re-confirmation (see above).

---

