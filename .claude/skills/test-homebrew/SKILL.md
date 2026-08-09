---
name: test-homebrew
description: Test a homebrew .nro on a real Nintendo Switch via the sys-botbase MCP tools — close the running game, launch hbmenu (album/applet, R-hold, or forwarder), send the .nro over NetLoader with nxlink, capture its nxlinkStdio() log, and return the console to a clean home menu. Use when asked to test/launch/run a homebrew, open hbmenu, send an nro, or verify a Switch build's log output.
---

This drives a real Nintendo Switch that's already reachable through the `mcp__sys-botbase__*`
tools (controller input, screenshots, system info) — there is nothing to build or launch locally.
The one piece that needed real code is packaged as
`.claude/skills/test-homebrew/nxlink-send.sh`, which sends an `.nro` over the network and
streams back its log. Everything else in this workflow is a sequence of MCP tool calls.

All paths below are relative to the repo root.

## Prerequisites

- The `sys-botbase` MCP server (`mcp-server/`) already connected to this session, with
  `SYS_BOTBASE_ALLOW_WRITES=1` — the tools that press buttons will silently refuse otherwise. See
  `mcp-server/README.md` for how the server is configured.
- The console and this machine on the same LAN.
- Docker Desktop, for `nxlink-send.sh` (uses `ghcr.io/d3fau4/nx-dev:latest`, pulled automatically
  on first use).
- The homebrew you're testing calls `nxlinkStdio()` if you want its log streamed back — otherwise
  the NetLoader transfer still works, you just won't see stdout.

## Run (agent path)

### 1. Close whatever's running

```
mcp__sys-botbase__system_info(fields=["titleId", "gameName"])
```

If non-null: `HOME` → screenshot to confirm the tile says "En uso" → `X` → `A` to confirm the
"¿Quieres cerrar el programa suspendido?" dialog. Re-check `system_info`; both fields should be
`null`.

### 2. Launch hbmenu

Three ways — pick based on what you need:

| Method | How | RAM | Notes |
|---|---|---|---|
| **Album (applet)** | Home menu → Álbum icon → `A` | ~128 MB (403 MiB reported total) | Fastest, no game involved. **Default choice.** Exits with a single `HOME` press — no game process left behind. |
| **Hold R + launch a game** | `press R` → `click A` on a game tile → `click A` again on the profile picker → `release R` once hbmenu is visible | Full game RAM (~3 GB) | Use when the homebrew needs more than applet-mode RAM. A crash here only kills the game process, not the console. Leaves the underlying game "En uso" — close it via step 1 afterward. |
| **Forwarder title** (`05A4999443B90000`) | Launch that title directly, if installed | Same as R-hold | Only if the console has the forwarder + piracy patches installed. |

For the Album method:

```
controller(action="sequence", sequence="DDOWN,W300,DRIGHT,W200,DRIGHT,W200,DRIGHT,W200")
screenshot()   # confirm focus ring is on "Álbum" before pressing A — see Gotchas
controller(action="click", buttons=["A"])
```

For R-hold:

```
controller(action="press", buttons=["R"])
controller(action="click", buttons=["A"])   # select the game tile
controller(action="click", buttons=["A"])   # confirm user profile
# screenshot until you see "hbmenu vX.Y.Z" / "nx-hbloader" on screen
controller(action="release", buttons=["R"])
```

### 3. NetLoader the .nro

In hbmenu, `Y` opens NetLoader and prints `IP Addr: ..., Port: ...` — you only need the IP;
`nxlink` always targets the fixed transfer port (28280), which is what that on-screen port is.

```
controller(action="click", buttons=["Y"])
screenshot()   # read the IP off "Waiting for nxlink to connect..."
```

```bash
bash .claude/skills/test-homebrew/nxlink-send.sh <switch-ip> "C:\path\to\file.nro"
```

Then screenshot the console — the homebrew's own log (build ID, its checks, `RESULT: ...`) prints
directly on screen, and `nxlink-send.sh` streams the same text back to your terminal via
`nxlinkStdio()`.

### 4. Exit cleanly

```
controller(action="click", buttons=["PLUS"])   # exits the running .nro back to hbmenu
```

- **Launched via Album**: `controller(action="click", buttons=["HOME"])` — done, applet closes
  itself. Verify with `system_info` (both fields `null`).
- **Launched via R-hold or forwarder**: `HOME` returns you to the home menu, but the underlying
  game is still "En uso" — repeat step 1 (`X` → `A`) to actually close it.

## Test

`nxlink-send.sh` was run against a real console this session with a sample `t_sysinfo.nro` (a
`sysconf`/memory-region self-check homebrew, logs to `sdmc:/horizon_gpu_tests/t_sysinfo.log`),
once from Album/applet mode (`RESULT: PASS (21/21)`, 403 MiB reported) and once from R-hold game
mode (`RESULT: PASS (21/21)`, 3189 MiB reported) — the RAM delta is the applet-vs-game-mode
ceiling in the Gotchas below, confirmed empirically rather than assumed.

---

## Gotchas

- **hbmenu's icon-row focus is stateful, not absolute.** A fixed `DDOWN,DRIGHT,DRIGHT,DRIGHT`
  sequence from the game tile does **not** reliably land on Álbum — the home menu remembers
  whatever icon was last focused (e.g. "Noticias" if you opened it before), so the same sequence
  can end up one or two icons off and open the wrong app. Always screenshot after the `DDOWN` and
  confirm the highlighted icon's label before pressing `A`.
- **`nxlink` needs `-s`, or the console's log connection times out.** Without it, the homebrew
  prints `nxlink: host <ip> knows about this run but the connection failed (errno 116) ... nxlink
  has to be run with -s for anything to be listening` on-screen, and your terminal shows nothing —
  the transfer itself still succeeds, only the log stream fails.
- **The log-server port must be published from the container.** `-s` alone isn't enough on Docker
  Desktop for Windows: the console connects back to the host's real LAN IP (Docker Desktop's
  network mode makes that address correct), but without `-p 28771:28771/tcp` there's nothing
  listening on that port from outside the container, so the connection still fails silently.
- **Git Bash mangles the container-side path.** `/data/file.nro` as a bare argument gets rewritten
  to `C:/Program Files/Git/data/file.nro` by MSYS's automatic path conversion before Docker ever
  sees it (`Failed to open C:/Program Files/Git/data/...`). `nxlink-send.sh` sets
  `MSYS_NO_PATHCONV=1` to stop this.
- **`B` in hbmenu navigates up a directory, not back out of the app.** At `sdmc:/switch` it goes to
  `sdmc:/`; there it just stops showing a "Back" hint. The only way to fully exit hbmenu is `HOME`.
- **Applet-mode RAM is a real ceiling, not a formality.** `svcGetInfo` reports ~403 MiB total in
  Album/applet mode vs ~3.2 GiB in R-hold/game mode (confirmed above) — a homebrew that allocates
  more than that will crash or fail to launch in applet mode specifically. If a `.nro` works via
  R-hold but not via Album, that's why.
- **A crash from R-hold only takes down the game process.** That's the whole reason to prefer
  R-hold/forwarder over the Album method when RAM headroom matters: closing the crashed game (step
  1) and retrying doesn't require rebooting the console.

## Troubleshooting

- **`Failed to open C:/Program Files/Git/data/...`**: Git Bash path conversion rewrote the
  container path. Fixed by `MSYS_NO_PATHCONV=1` — already set inside `nxlink-send.sh`; if you're
  running `docker run ... nxlink` by hand instead, set it yourself.
- **On-screen: `nxlink: host X knows about this run but the connection failed (errno 116)`**:
  missing `-s` on the `nxlink` invocation, or the log port isn't published from the container.
  `nxlink-send.sh` sets both; if you still see this, check `docker ps` for a stale container still
  holding port 28771 (`docker stop <id>` it and retry).
- **"¿Quién va a usar el programa?" profile picker appears after launching via R-hold**: normal —
  it's the standard user-profile prompt any title shows on launch. `A` accepts the highlighted
  profile; R must still be held through this screen.
- **`docker ps` shows the nxlink container still `Up` after the homebrew shows "Press + to exit."**:
  expected — `-s` keeps the log server alive until the homebrew's stdio socket disconnects. Press
  `+` on the console to let the app exit; the container (run with `--rm`) then prints the final
  full-log dump and removes itself.
