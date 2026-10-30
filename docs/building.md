# Building, running and testing

The README's build steps, in Git Bash on Windows, and what an agent needs beyond them.

## A fresh checkout

`tools/env.sh` (or `tools/env.ps1`) puts a per-user toolchain on `PATH` (its header says what it expects). llvm.org's
clang has no MIPS target, so the patches library and mods use MSYS2's clang with llvm.org's `ld.lld` (MSYS2's `ld.lld`
rejects `--emit-relocs`).

A fresh checkout has none of the generated files: update the submodules (`git -c core.longpaths=true submodule update
--init --recursive`), run `tools/apply_runtime_patches.sh`, extract the ROM (`roms/`), make the venv (`.venv`, which
`iterate.sh` runs), clone n64sym into `ref/` for `gen_syms.py`, `touch portable.txt` to keep settings, saves and mods
in the checkout, then `tools/recompile.sh`, the README's configure line and `bash tools/iterate.sh 30`.

Copying the submodules from another checkout is faster than cloning them: `bash tools/copy_submodules.sh <checkout>`,
then the patch script. Never copy `lib/` by hand: its `.git` files point into the other checkout's git dir, so the two
share one, and git in either acts on the other's files. If `git -C lib/rt64 rev-parse --show-toplevel` names another
checkout, the same script with no argument gives this one its own (its header says how). If `git status` then fails
with `bad object HEAD` in a nested submodule, a fetch hit Windows' path limit; repair it with
`git -c core.longpaths=true submodule foreach --recursive 'git config core.longpaths true; git cat-file -e $sha1 || git fetch origin $sha1'`.

## Building on a Linux host

`tools/remote_build.sh` (its header says how) builds over ssh instead (`BUILD_HOST`), in a Docker image of the tools,
`tools/docker/Dockerfile`: it sends a checkout's HEAD and its uncommitted changes to a copy of its own there, `web`
builds the game's page in that copy's `build-web/`, `get build-web <dir>` brings it back, and `-C <checkout>` does
`sync`, `run` and `get` for another repo, a game fork. A tool a build lacks goes into the Dockerfile.

## Running the game

`iterate.sh <secs>` boots with `--boot --stacks` into `run.log`, and `ninja -C build Rush2049Recompiled` is enough after
a source change. To look at the game, capture its window, not the screen: `PrintWindow(hwnd, hdc, 3)` on the window of
the process you started, from a DPI-aware process (`SetProcessDPIAware()` first, or a 1280x960 window comes back as
731x548).

## Developer flags

```
--boot                       skip the launcher (needs the ROM already stored)
--show-console               the log to a console (Windows: opens one; a redirect gets it too) or, on Linux, to a
                             redirect, which without it gets nothing; otherwise it goes to rush2049.log in the app
                             folder, the crash trace with it (Linux: unless started from a terminal)
--stacks / --probes          every few seconds: every thread's stack and the probes / the probes only
--players <n>                ports 2..n copy port 1 (and --input), to drive split screen with one pad
--input <file>               scripted port 1 input: "<seconds> <buttons hex> [x y]" lines; 0x10000 is Rewind
--window <w>x<h>             window size (default 1600x1200; Auto resolution follows it)
--musyx hle|native|lle|compare   audio path; native is the HLE without the 44.1 kHz mix
--dump-audio <wav>           the game's mix before resampling (float); --dump-output <wav>: what the device gets
--dump-dl <file> <seconds>   one frame's display list as text
--dump-ram <prefix> <t1,..>  all of RDRAM at those times; diff two to find a setting's byte
--watch <addr>               stacks on the first 6 changes of one RDRAM byte; --hwwatch <addr>: reads too
--profile <after> <secs>     sample every thread each ms, print top functions (Windows)
--gpu-profile <after> <secs> RT64's GPU time per frame and per present, and per part of each
```

How to read `--gpu-profile` (in cycles, runs interleaved, the host's load beside each number) and where a frame goes on
an RTX 2070 are beside `start_gpu_profiler` in `src/main.cpp`.

## Testing in the game

- The cheat menu (in the README) unlocks every track.
- Ghost Race (mode word `0x8014A110` == 2) replays the player's inputs, recorded only for a race finished inside the
  time limit, and offered as GHOST 1 in the *next* Ghost Race on that track. The SAVE GHOST prompt, which writes it to
  the pak for later starts, needs a profile on the pak (not Just Play) and the Save Ghosts setting (Original: a
  developer profile name only). To end a race in seconds, write a small float (12.0) into the track's time limit in the
  table at `0x80111754` before it starts, and at its end (`func_800D510C` at 0x800D5208, `v0` the car's result) set
  the finished byte +0xEF and a time below the limit at +0xF0: it then counts as finished inside the limit.
- To make a lap in seconds, give player 1's car (`0x8014A250`, the next car 0x808 on) the finish as its next
  checkpoint a few seconds into the race: the halfword +0x7E2 (checkpoint) to the last one, +0x7E4 (next) to the
  finish, and the byte +0x7DC to 1 (`func_800D24C8` steps them; the track's table is at `0x80151CE8`, the finish at
  +4 and the count at +8). The car makes the lap a few seconds on, the lap box shows its time and the HUD lap 2.
- Every port reports a controller with a pak; a port nobody plays reads as a pad left alone.
