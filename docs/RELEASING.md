# Releasing

There are two kinds of release here, and the difference between them is the difference
this project is organised around: **what compiled** versus **what ran on a console**.

## The automatic one — cross-built, unverified

CI runs on GitHub Actions now: [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) on
every push, pull request and manual dispatch, and
[`.github/workflows/release.yml`](../.github/workflows/release.yml) on a `v*` tag or manual
dispatch. The Forgejo instance's own workflows are kept, commented, in
[`.forgejo/workflows-disabled/`](../.forgejo/workflows-disabled/) — this project is developed
against both forges, and re-enabling CI there is a separate decision from enabling it here.
`scripts/ci-forgejo-release.sh` still uploads a package through the Forgejo API for that
instance; `scripts/ci-github-release.sh` does the GitHub equivalent through `gh`.

Both workflows build **on the runner itself**, not inside a `container:` — the toolchain image
is built or pulled once, then driven per-command through `scripts/toolchain-env.sh`'s own
`horizon_run` (unset `$DEVKITPRO` is what turns this on; it is the same split
`scripts/build-switch.sh` already uses for the Makefile path). The image still comes from
`ghcr.io/<owner>/nx-dev-mesa` — HTTPS, unlike the Forgejo instance (see `STATUS.md`, "CI
switched off, and what is kept instead") — pushed there once per Dockerfile/pin change and
pulled on every run after. `scripts/build-toolchain-image.sh` needed no change for any of this —
pointing `$HORIZON_NX_DERIVED_IMAGE` at the pulled tag is the whole mechanism.

Then `scripts/package-horizon.sh`, and `release.yml` publishes:

- `mesa-nvk-horizon-<tag>-nro.tar.gz`, containing:
  - `lib/libhorizon_gpu.a` and `lib/libhorizon_compat.a` — what another project actually links
    against,
  - `include/horizon_gpu/` — the public headers those libraries are compiled against; pre-1.0,
    both are versioned together and neither is meaningful without the other,
  - `lib/nvk/` — the seventeen archives Mesa's own build produces for the NVK Vulkan driver
    (`libnvk.a`, `libvulkan_wsi.a`, and the rest of `$HORIZON_NVK_TEST_LIBS`), under their own
    path so the same-named-but-different `libmesa_util.a`/`libmesa_util_c11.a` this build also
    produces for tests 12/13 cannot collide with it. Present only when this build actually
    configured NVK — the Makefile-only path never does (`docs/BUILDING.md` §4) — and linked
    exactly the way `meson.build`'s `nvk_whole_libs`/`nvk_test_libs` and the `t_vk_*` tests
    already link them; nothing here documents a link line beyond pointing at those,
  - every `horizon_gpu` test `.nro` the tree currently names in `meson.build` (around 34,
    fourteen of them `t_vk_*` exercising NVK) — this project's own tests, not something a
    consumer embeds, kept in the package because they are the evidence the libraries in the
    same tarball actually work,
  - `LICENSE`, `LICENSES.md` and `MANIFEST.txt`,
- its `.sha256`,
- release notes carrying the build id and the caveats below.

The licence files are in there because MIT requires its notice to accompany copies of
the software, and a tarball of built artefacts is a copy. `package-horizon.sh` puts them
in, so a package built by hand is no different.

**What it is not.** Nothing in that package has run on a console. The notes say so, in
the release body, because a reader who skips `STATUS.md` should still not come away
thinking otherwise. A `.nro` that compiles is not a `.nro` that works.

**Nothing is left out any more.** The `release.yml` this workflow replaces on Forgejo
shipped only the eighteen driver-free `.nro`, because the image it ran in carried no Mesa
checkout. That gap does not exist here: this workflow's toolchain image is the same one
`ci.yml` builds and runs the full `scripts/ci-build-archives.sh` in — Mesa, the Rust half
and NVK — so the package includes every archive the tests link, `t_vk_*` included.
Publishing less than what CI itself already validated would be the wrong kind of gap to
leave unnamed.

`workflow_dispatch` runs the build and packaging **without publishing**, which is the way
to check the pipeline without cutting anything: it builds, packages, proves the manifest
identifies its toolchain, and attaches the tarball to the run as an artefact. Publishing
is guarded on the ref being a `v*` tag, because on a manual run `$GITHUB_REF_NAME` is the
branch — without that guard, a dispatch from `main` would publish a release called
`main`.

`scripts/ci-build-archives.sh` is the other half, and worth re-running rather than
debugging on the first failure: it fetches from four external services, so a red run is
as likely to be somebody else's CDN as a broken tree. Its network steps already retry
three times for that reason.

To publish by hand instead of through the workflow — reproducing exactly what
`release.yml`'s `publish` step does. `package-horizon.sh` fills `build/pkg` with the `.nro`,
licences and `MANIFEST.txt`; it does not tar them, and `NOTES.md` is not generated for you
outside the workflow, so both are yours to make here:

```sh
scripts/package-horizon.sh build/pkg
tar -czf mesa-nvk-horizon-v0.1.0-nro.tar.gz -C build/pkg .
sha256sum mesa-nvk-horizon-v0.1.0-nro.tar.gz > mesa-nvk-horizon-v0.1.0-nro.tar.gz.sha256
# write NOTES.md — see "What a release may claim" below for what it must say
GH_TOKEN=<token with contents: write> \
    scripts/ci-github-release.sh v0.1.0 NOTES.md \
        mesa-nvk-horizon-v0.1.0-nro.tar.gz mesa-nvk-horizon-v0.1.0-nro.tar.gz.sha256
```

The Forgejo equivalent is still `scripts/ci-forgejo-release.sh` (see the manual section
below for its full invocation, including the environment it reads).

## The manual one — with hardware behind it

This is the release worth making, and it cannot be automated: no CI runner has a Nintendo
Switch attached.

1. **Build everything**, including NVK — [`BUILDING.md`](BUILDING.md) §4. Up to 34
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
7. **Tag, and publish it yourself.** Nothing does it for you:

   ```sh
   git tag -a v0.1.0 -m "…"
   git push origin v0.1.0
   tar -czf mesa-nvk-horizon-v0.1.0-nro.tar.gz -C build/pkg .
   sha256sum mesa-nvk-horizon-v0.1.0-nro.tar.gz > mesa-nvk-horizon-v0.1.0-nro.tar.gz.sha256
   GITHUB_API_URL=<instance>/api/v1 GITHUB_REPOSITORY=<owner>/<repo> \
   FORGEJO_TOKEN=<token with write:repository> \
       scripts/ci-forgejo-release.sh v0.1.0 NOTES.md \
           mesa-nvk-horizon-v0.1.0-nro.tar.gz mesa-nvk-horizon-v0.1.0-nro.tar.gz.sha256
   ```

   Say in the notes which tests ran on hardware, on which firmware, and in which memory
   mode, and link the logs.

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
- what has never been verified — currently docked resolution, two surfaces over two
  `NWindow`s, and `VK_SUBOPTIMAL_KHR` ever being *returned* (run 21 measured the rule
  around it over 2303 frames, but its section D needs a hand on the dock and has never
  executed);
- the known failures from `STATUS.md`, not a summary of them;
- that this is pre-1.0 software driving a GPU through system services, and that it has
  taken a console down before.

## Versioning

`meson.build` declares `version : '0.1.0'`. There has been no release yet, so nothing is
pinned to that number by anyone else; keep the tag and that field in step.

Pre-1.0 means the interfaces in `horizon/include/horizon_gpu/` may change. When they do,
say so in the notes — `nvkmd_horizon` in `mesa-patches/` is the only consumer, and it is
in this repository, but it will not stay that way forever.
