# Building

Three things can be built here, and they cost wildly different amounts:

| What | Needs | Roughly |
|---|---|---|
| **Host tests** — `horizon/`'s pure logic | a C compiler | seconds |
| **The 18 `horizon_gpu` test `.nro`** | devkitA64, or Docker | seconds to a minute |
| **The full NVK driver and its 14 Vulkan test `.nro`** | the above, plus a derived toolchain image with Rust, libclang and LLVM | tens of minutes, and several GB |

Start at the top. Each row is useful on its own, and the first needs nothing installed.

---

## 1. Host tests — no toolchain, no console

```sh
scripts/run-host-tests.sh
```

Compiles six suites against the *same* `horizon/` sources the Switch build compiles —
alignment and overflow arithmetic, the VA interval set, wrap-safe syncpoint comparison,
the GPFIFO command emitters, status strings and logging — with `-Wall -Wextra -Werror`
and, where the compiler supports them, ASan and UBSan. Each prints `RESULT: PASS (n/n)`.

This works because those modules are libnx-free by design, which
[`scripts/check-layering.sh`](../scripts/check-layering.sh) enforces. It is also what CI
runs on every push.

`CC` and `OUT` are honoured (`CC=clang scripts/run-host-tests.sh`).

## 2. The cross build

### Prerequisites: pick one

**devkitA64 installed** — set `$DEVKITPRO` (usually `/opt/devkitpro`) and you are done.
The build resolves everything through that variable; there are no machine-specific paths
anywhere, and [`scripts/check-no-abs-paths.sh`](../scripts/check-no-abs-paths.sh) fails
the build if one appears.

**No devkitA64** — Docker is enough. The scripts fall back to
`ghcr.io/d3fau4/nx-dev:latest`, whose `/opt/devkitpro` layout matches a normal install.
This is the supported path when devkitPro's package servers are unreachable, which is
common behind a restrictive proxy while `ghcr.io` still works. Override the image with
`HORIZON_NX_IMAGE`.

### The quick way

```sh
scripts/build-switch.sh -j4     # runs make directly, or inside the container
```

Output: 18 `.nro` in `build/`. That wrapper is one `exec make` when `$DEVKITPRO` is set,
and a `docker run` mounting the tree at the same path inside and out when it is not — so
compiler diagnostics point at the files you actually have.

### The two cross-build paths, and why they differ

There are two, deliberately, and they do **not** build the same set:

| | `Makefile` | Meson (`meson.build`) |
|---|---|---|
| Entry point | `make` / `scripts/build-switch.sh` | `scripts/configure-horizon.sh` then `scripts/build-horizon.sh` |
| `libhorizon_gpu.a` | yes | yes |
| The 18 `horizon_gpu` tests | yes | yes |
| `t_threads`, `t_ostime` (need Mesa core) | yes, when Mesa is built | yes, when Mesa is built |
| The 14 `t_vk_*` Vulkan tests | **no** | yes, when NVK is built |
| Maximum `.nro` | 20 | **34** |

The Makefile is the path whose output was verified on hardware, and it is readable
without running anything; the Meson path is the one that can link NVK.
[`scripts/check-mesa-test-parity.sh`](../scripts/check-mesa-test-parity.sh) fails if the
two ever disagree about the tests they *share* — the Vulkan tests are explicitly outside
its comparison, because only one path builds them.

Both are self-contained: `make` builds `compat/` and the build-id header itself, so
`make clean && make` works from a bare clone.

### Meson path

```sh
scripts/configure-horizon.sh
scripts/build-horizon.sh
```

`configure-horizon.sh` installs the pinned Meson, generates the machine-specific half of
the cross file into `build/toolchain/` (gitignored by construction), builds
`libhorizon_compat.a` — which must exist *before* `meson setup`, because the cross file
names `-lhorizon_compat` and Meson links a sanity program during its compiler check —
and then configures. It is idempotent, and it wipes and reconfigures by itself when the
cross file or `meson.options` changed, since Meson only reads those on a first configure.

`meson test` does nothing here, on purpose: these binaries run on a Nintendo Switch, not
on the build machine.

## 3. Mesa's non-driver core

Two tests, `t_threads` and `t_ostime`, measure Mesa's *own* code on hardware — the C11
threads shim and `os_time.c`. They link the archives Mesa's build produced rather than
recompiling those sources with flags of our own, because the object under test has to be
the object Mesa builds.

```sh
scripts/fetch-mesa.sh              # mesa/ at the pinned MESA_COMMIT
scripts/apply-mesa-patches.sh      # the 75-patch series
scripts/configure-mesa.sh
scripts/build-mesa.sh
```

Then rebuild: both build paths notice the archives have appeared and stop skipping the
two tests. `$MESA_BUILD_DIR` (default `build/mesa-probe`) selects where, and is honoured
by every consumer.

`scripts/fetch-mesa.sh` clones `mesa/` at `MESA_COMMIT` and verifies the SHA it got,
falling back to GitLab's archive addressed *by commit* if git-over-https is blocked.
`mesa/` is gitignored: it is a pinned checkout, never our source.

`scripts/apply-mesa-patches.sh` is idempotent and non-destructive — a second run says
`all N patches already applied`. It identifies each patch by its subject **and** its
`git patch-id`, so an edited diff under an unchanged subject is reported as divergence
rather than silently skipped. `--list` shows the state without touching anything.

## 4. The full NVK driver

This is the expensive one. It needs a Rust sysroot for the Switch target, `bindgen`,
`cbindgen`, `libclang`, an LLVM-15 `libclc` closure and SPIRV-Tools — none of which
devkitPro ships. In container mode all of that lives in a *derived* image built on top
of `nx-dev`.

```sh
# Material first: containers have no network, including the one `docker build` uses.
scripts/fetch-rust-tools.sh
scripts/fetch-rust-crates.sh
scripts/fetch-clc-deps.sh
scripts/fetch-mesa-subprojects.sh

scripts/build-toolchain-image.sh    # container mode only; --force to rebuild
#   with a local devkitA64 instead:
#   scripts/build-rust-sysroot.sh

scripts/build-mesa-clc.sh           # native tools for the build machine
scripts/build-mesa-nvk.sh           # configures itself if needed
```

Then `scripts/build-horizon.sh` again, and the 14 `t_vk_*` `.nro` appear.

`$MESA_NVK_BUILD_DIR` (default `build/mesa-nvk`) selects where the driver is built and
is passed through to Meson as `-Dnvk_build_dir`. Set it for *both* the driver build and
the test build, or the tests will look somewhere the driver was never built and skip
themselves.

## 5. Packaging

```sh
scripts/package-horizon.sh [outdir]      # default build/pkg
```

Collects the `.nro` into one directory with a `MANIFEST.txt` that ties each artefact to
the toolchain that produced it: a sha256 per file, the resolved image digest, the live
devkitA64/libnx package versions, and the **build id read back out of the binaries** —
not out of the source tree. It refuses a package holding more than one build id, or an
artefact carrying none.

That refusal exists because a `.nro` on an SD card looks exactly like the one it
replaced, and an afternoon was once spent measuring a three-day-old build.

## 6. Everything is pinned in one file

[`toolchain/versions.env`](../toolchain/versions.env) is the single source of truth, and
it is split deliberately:

- **Environment** — the Switch toolchain itself (devkitA64, libnx). Not pinned, because
  devkitPro does not support pinning in the usual sense; see
  [`docs/known-risks.md`](known-risks.md) R15. It is *recorded* at packaging time
  instead, which is what makes a hardware result attributable.
- **Pinned** — everything this project chose: Mesa `mesa-26.1.5` at commit
  `6a02618ccf6c5651ecb9cccbde571eb61fd73592`, Meson 1.11.2, mako, PyYAML, bindgen,
  cbindgen, the LLVM debs with their sha256, SPIRV-Tools, and the Rust target triple.

The Mesa *commit* is pinned, not just the tag, because tags can be moved — and
`fetch-mesa.sh` verifies the SHA it actually got.

`scripts/print-toolchain-versions.sh` reports what the current environment resolves to,
read-only.

## 7. The gates

Five run in CI on every push and pull request, as the opening steps of
[`.forgejo/workflows/archives.yml`](../.forgejo/workflows/archives.yml) — the one
workflow there is. They cost seconds against that job's several minutes, so they come
first:

| Gate | What it refuses |
|---|---|
| `check-layering.sh` | `horizon/` reaching for Vulkan, Mesa or `nwindow`; `compat/` reaching past newlib and libnx |
| `check-no-abs-paths.sh` | `/home/`, `/Users/`, `/work`, a drive letter or a mount point in the build files |
| `check-history-intact.sh` | an undeclared edit to `docs/history/` or `docs/hw-logs/` — every file's sha256 is in a manifest |
| `check-mesa-test-parity.sh` | the Makefile and the Meson build drifting apart on tests, archives, defines or includes |
| `run-host-tests.sh` | a regression in the pure logic |

Two more need built artefacts, so they used to be manual. The `archives` job produces
exactly those artefacts, so it runs them too — `scripts/ci-build-archives.sh` ends with
both:

- `check-dispatch-complete.sh` — reads the generated Vulkan dispatch table out of a
  linked Horizon ELF.
- `check-tls-relocs.sh` — scans devkitA64 objects for the TLS miscompile described in
  [`docs/devkita64-tls-report.md`](devkita64-tls-report.md).

Run them by hand after a build if you want them sooner than CI.

`check-rust-target.sh` compares the checked-in target JSON against what `rustc` reports,
and needs a nightly toolchain.

## 8. The scripts, in one table

All 36 live in [`scripts/`](../scripts/), are `set -eu`, and `cd` to the repository root
themselves — so they can be run from anywhere as `scripts/<name>.sh`.

**Fetch** (host-side, because containers have no network)
`fetch-mesa.sh` · `fetch-mesa-subprojects.sh` · `fetch-rust-tools.sh` ·
`fetch-rust-crates.sh` · `fetch-clc-deps.sh`

**Configure**
`configure-horizon.sh` · `configure-mesa.sh` · `configure-mesa-nvk.sh` ·
`gen-cross-file.sh` · `gen-build-id.sh`

**Build**
`build-switch.sh` (the Makefile path) · `build-horizon.sh` · `build-compat.sh` ·
`build-mesa.sh` · `build-mesa-nvk.sh` · `build-mesa-clc.sh` ·
`build-rust-sysroot.sh` · `build-toolchain-image.sh`

**Check**
`check-layering.sh` · `check-no-abs-paths.sh` · `check-history-intact.sh` ·
`check-mesa-test-parity.sh` · `check-dispatch-complete.sh` · `check-tls-relocs.sh` ·
`check-rust-target.sh` · `run-host-tests.sh`

**Package and tooling**
`package-horizon.sh` · `print-toolchain-versions.sh` · `apply-mesa-patches.sh` ·
`split-status.py` · `spv-embed.py` · `toolchain-env.sh` (sourced, never executed — it
resolves local-vs-container mode and is what every other script agrees through)

**CI**
`ci-build-archives.sh` — the whole chain above in one command, with retries around the
network steps, ending in a check that every archive exists and that every `.nro` meson.build names links
them. Run it by hand to reproduce what CI does · `ci-require-host-tools.sh` and
`ci-require-docker.sh` — the two preflights it starts with ·
`ci-forgejo-release.sh` — creates a release and uploads assets through the Forgejo API

### What has to be on the machine itself

Most of the build happens inside the toolchain image, but the **downloads do not**: the
image has no network, so everything fetched is fetched out here and mounted in. That
makes a short list of host requirements, which `scripts/ci-require-host-tools.sh` checks
before anything slow starts:

`git` · `python3` · `curl` · `tar` · `sha256sum` · **`cargo`**

`cargo` is the one that catches people out — `scripts/fetch-rust-tools.sh` runs
`cargo vendor` to collect bindgen's and cbindgen's sources. Nothing is *compiled* with
it, so any recent toolchain does.

None of that applies when you work **inside** the toolchain image, which is what CI does
and what you can do too:

```sh
docker run --rm -e DEVKITPRO=/opt/devkitpro \
    -v "$PWD":"$PWD" -w "$PWD" <toolchain-image> \
    scripts/ci-build-archives.sh
```

`$DEVKITPRO` is set in there, so `scripts/toolchain-env.sh` takes the local path: no
nested containers, no mounts of its own, and cargo, bindgen, cbindgen, LLVM and the Rust
sysroot are already present. Do not use a **login** shell for this — `/etc/profile`
rewrites `PATH` and drops `/opt/cargo/bin`, and the preflight will tell you cargo is
missing on an image that has it.

### The derived image, and not building it every time

The base image is not enough for Mesa's Rust half: `bindgen`, `cbindgen`, `clang-15`
and `llvm-config` are all absent from it. `scripts/build-toolchain-image.sh` adds them,
which costs about **eleven minutes and 7.3 GB**.

That is paid once per machine — the script recognises an image it has already built by
an identity label covering the base image's ID, the pinned bindgen/cbindgen/LLVM
versions, the resolved `.deb` closure and the Dockerfile's own digest.

**CI does not publish it anywhere.** The first job of each workflow builds it on the
runner, where it stays in that machine's docker daemon; the job that does the work then
names it and finds it locally. Publishing was tried and abandoned: the instance speaks
plain HTTP, docker refuses a registry that is not HTTPS, and configuring an exception on
the daemon is a lot of ceremony for an image that never leaves the machine. If a job
cannot resolve the image, the job that builds it is the log to read.

Point somewhere else with the `TOOLCHAIN_IMAGE` repository variable, or by hand:

```sh
HORIZON_NX_DERIVED_IMAGE=<some-other>/nx-dev-mesa:latest \
    scripts/ci-build-archives.sh
```

**A pull is a candidate, not an answer.** The identity check runs against whatever is
there, published or local, and rebuilds when it does not match. A published image that
no longer corresponds to `toolchain/Dockerfile` cannot be used silently.

**The image says what it is.** `toolchain/Dockerfile` writes its build identity to
`/etc/mesa-nvk-horizon-toolchain`, and `scripts/package-horizon.sh` reads it — so a
package built *inside* the image still records the toolchain behind it, where it used to
record nothing but `local`. The two probes that prove the image works (bindgen against a
real header, `rustc` against the sysroot) are `RUN` steps in that Dockerfile, so a broken
toolchain fails the build instead of producing an image somebody has to un-tag.

## 9. Environment variables

| Variable | Default | Effect |
|---|---|---|
| `DEVKITPRO` | unset | A local devkitA64. When set, no container is used anywhere. |
| `HORIZON_NX_IMAGE` | `ghcr.io/d3fau4/nx-dev:latest` | The toolchain container. |
| `MESA_BUILD_DIR` | `build/mesa-probe` | Where Mesa's core was built; selects whether `t_threads`/`t_ostime` are built. |
| `MESA_NVK_BUILD_DIR` | `build/mesa-nvk` | Where NVK was built; selects whether the 14 `t_vk_*` are built. |
| `HORIZON_BUILD_DIR` | `build/meson` | The Meson build directory. |
| `CC`, `OUT` | `cc`, `build/host-tests` | Host tests only. |

## 10. Cleaning

`make clean` removes `build/` **except** `build/toolchain` and the Mesa build directory:
the pinned Meson and Mesa's Python dependencies are installed over the network, and
Mesa takes a long time to build. Deleting either by hand is safe; it just costs time.

---

Once something is built, [`USAGE.md`](USAGE.md) covers getting it onto a console and
reporting what it did.
