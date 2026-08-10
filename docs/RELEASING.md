# Releasing

There are two kinds of release here, and the difference between them is the difference
this project is organised around: **what compiled** versus **what ran on a console**.

## The automatic one — cross-built, unverified

Pushing a tag matching `v*` runs
[`.forgejo/workflows/release.yml`](../.forgejo/workflows/release.yml):

```sh
git tag -a v0.1.0 -m "…"
git push origin v0.1.0
```

It builds the Makefile path *driving* `ghcr.io/d3fau4/nx-dev:latest` from the runner —
not inside it, which matters: a step running inside the image has `$DEVKITPRO` set, and
`scripts/toolchain-env.sh` then reports local mode, so the manifest would record
`image: local` and lose the digest of the image that actually produced the binaries.
The workflow refuses to publish a package whose manifest carries no `@sha256:`.

Then `scripts/package-horizon.sh`, and it publishes:

- `mesa-nvk-horizon-<tag>-nro.tar.gz` — the 18 `horizon_gpu` test `.nro`, `LICENSE`,
  `LICENSES.md` and `MANIFEST.txt`,
- its `.sha256`,
- release notes carrying the build id and the caveats below.

The licence files are in there because MIT requires its notice to accompany copies of
the software, and a tarball of built artefacts is a copy. `package-horizon.sh` puts them
in, so a package built by hand is no different.

**What it is not.** Nothing in that package has run on a console. The notes say so, in
the release body, because a reader who skips `STATUS.md` should still not come away
thinking otherwise. A `.nro` that compiles is not a `.nro` that works.

**What it leaves out, and why.** The thirteen `t_vk_*` tests and the NVK driver itself are
absent. They need the derived toolchain image — libclang, `bindgen`, `cbindgen`, a Rust
sysroot for the Switch target, an LLVM-15 `libclc` closure — plus a full Mesa build, with
material fetched outside the container because containers here have no network. That is
tens of minutes and several GB, and it is not something a tag push should quietly start.
The gap is named in the workflow header and in the release notes rather than left to look
like completeness.

`workflow_dispatch` runs the build and packaging **without publishing**, which is the way
to check the pipeline without cutting anything: it builds, packages, proves the manifest
identifies its toolchain, and attaches the tarball to the run as an artefact. Publishing
is guarded on the ref being a `v*` tag, because on a manual run `$GITHUB_REF_NAME` is the
branch — without that guard, a dispatch from `main` would publish a release called
`main`.

[`archives.yml`](../.forgejo/workflows/archives.yml) is manually runnable too, and for
the same kind of reason: it fetches from four external services, so when it goes red,
pressing the button again is how you tell a broken tree from a bad afternoon on somebody
else's CDN.

## The manual one — with hardware behind it

This is the release worth making, and it cannot be automated: no CI runner has a Nintendo
Switch attached.

1. **Build everything**, including NVK — [`BUILDING.md`](BUILDING.md) §4. Up to 33
   `.nro`.
2. **Package it**, and let the manifest do its job:

   ```sh
   scripts/package-horizon.sh build/pkg
   ```

   It refuses a package holding more than one build id, or an artefact carrying none.
   Both refusals were provoked and observed before the gate was believed; do not work
   around them.
3. **Note the build id** from `build/pkg/MANIFEST.txt`. Everything below is a claim about
   that stamp and nothing else.
4. **Run the tests on a console**, in the order in [`../tests/README.md`](../tests/README.md).
5. **Commit the logs** to `docs/hw-logs/`, verbatim, with their digests recorded:

   ```sh
   scripts/split-status.py --record-logs
   scripts/check-history-intact.sh
   ```

   Failing logs go in too. They are evidence, and a directory holding only successes is
   not a record.
6. **Update `STATUS.md`** — what ran, what passed, what did not, on which build id.
7. **Tag and push.** The workflow publishes the cross-built package; edit the release
   afterwards to attach the full set and to say which tests ran on hardware, on which
   firmware, and in which memory mode. Link the logs.

## What a release may claim

Use these words and no stronger ones:

| Wording | Means |
|---|---|
| *built* | It compiled. Nothing else. |
| *cross-built* | It compiled for `aarch64` Horizon and a `.nro` exists. |
| *verified on hardware* | It ran on a real console and the log is in `docs/hw-logs/`. |

Never write "supported", "stable" or "works" without saying which of the three it is and
pointing at the evidence. `STATUS.md`'s *Current state* block is the model: every claim
in it carries its number and the run that produced it.

Also state, in any release notes:

- the build id, which is the only thing that makes a bug report attributable;
- what has never been verified — currently docked resolution,
  `VK_PRESENT_MODE_IMMEDIATE_KHR` through Vulkan, two surfaces over two `NWindow`s, and
  `VK_SUBOPTIMAL_KHR` ever being *returned* (run 21 measured the rule around it over
  2303 frames, but its section D needs a hand on the dock and has never executed);
- the known failures from `STATUS.md`, not a summary of them;
- that this is pre-1.0 software driving a GPU through system services, and that it has
  taken a console down before.

## Versioning

`meson.build` declares `version : '0.1.0'`. There has been no release yet, so nothing is
pinned to that number by anyone else; keep the tag and that field in step.

Pre-1.0 means the interfaces in `horizon/include/horizon_gpu/` may change. When they do,
say so in the notes — `nvkmd_horizon` in `mesa-patches/` is the only consumer, and it is
in this repository, but it will not stay that way forever.
