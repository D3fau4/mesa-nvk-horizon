# Security policy

## What this project is, and what that means for its threat model

`mesa-nvk-horizon` is a GPU driver backend for homebrew on the Nintendo Switch. It has
no network surface, no privileged daemon, no server component and no persistent state
beyond log files it writes to the SD card. It runs as ordinary homebrew, under whatever
permissions the launching environment already granted.

So the realistic failure mode is not a remote attacker. It is **a bug that takes the
console down**, because this code drives the GPU and talks to the `nv` system services
directly. That has already happened once here:
`vkDestroySwapchainKHR` on a superseded swapchain disconnected the window from the
swapchain that had replaced it and killed `qlaunch` with fatal `0x290` — the hardware
log was kept deliberately, and the bug was fixed by patch `0070`.

## Supported versions

There is no released version yet, so only the current `main` branch is supported. This
project keeps *host build*, *cross build* and *verified on real hardware* strictly
apart: a claim about console behaviour needs a log behind it, and a successful compile
is never one.

## Reporting a vulnerability

**Contact the maintainer directly and privately.** This project is hosted on Forgejo,
which has no private-advisory workflow, so there is no button to press — send a direct
message to the repository owner on the instance, or use any contact address listed on
their profile. Please do not open a public issue for something that could brick or
crash somebody's console until it has been looked at.

If you have no private channel and the issue is serious, open an issue saying only
*that* you have found something and how to reach you, with no details.

Include, as far as you have it:

- The `note horizon-build-id …` line the tests print, or the commit you built.
- Whether it was real hardware or an emulator, and the CFW version.
- The `.nro` and the sequence that provoked it.
- The console log, and the fatal report from `sdmc:/atmosphere/fatal_reports/` if the
  console produced one.

Expect an acknowledgement within a couple of weeks. This is a spare-time project and
there is no on-call rotation; be patient, and say so in the report if you have a
disclosure deadline.

## What is in scope

- Anything in this repository that can crash, hang or corrupt state on a console
  running the code as intended: `horizon/`, `compat/`, the `nvkmd_horizon` and WSI
  patches in `mesa-patches/`, and the build and packaging scripts.
- Memory-safety defects reachable from a Vulkan application using the driver.
- A build or packaging script that could execute or fetch something it should not.

## What is out of scope

- Anything requiring Nintendo's own code, keys or firmware to demonstrate. This project
  contains none of it and will not accept a report that depends on it.
- Bypassing custom-firmware or homebrew restrictions, or anything whose purpose is to
  run unauthorised commercial software. Not what this is for.
- Vulnerabilities in the dependencies themselves — Mesa, libnx, devkitA64, Atmosphère.
  Report those to their own projects; if the bug is in how *we* use them, that is in
  scope and we want it.
- "The driver crashes on a console" as a general statement: that is an ordinary bug and
  belongs in a public issue with its log. This project is pre-1.0 and says so.

## Legal

This project contains no Nintendo code, no NVIDIA proprietary blobs and no copyrighted
firmware. It targets homebrew execution environments on hardware the user owns.
