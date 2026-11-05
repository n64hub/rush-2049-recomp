# CLAUDE.md

A static recompilation of the N64 *San Francisco Rush 2049* (US) with N64Recomp, N64ModernRuntime, RecompFrontend
and RT64, plus enhancements. Almost all of it is written by AI agents, so this file is what the next one needs: the
rules, how work ships, and where everything else is written.

## What lives where

This repo is the game: its recompilation, enhancements and settings, the desktop build and its releases, and its own
web page (how it is built and published, its prebuilt shader list, its play plan).

The web support every recomp's page shares is N64Recomp-browser (`lib/N64Recomp-browser`): the forks of rt64, plume,
hlslpp, N64Recomp, N64ModernRuntime and RecompFrontend, the frontend, and the other games' web forks. It is developed
and documented there (its CLAUDE.md for the rules and The pins, its `docs/` for the how-to), and a game-agnostic web
change is made there and reaches this game when a pull request here moves the pin (`docs/releasing.md`).

## Where things are written down

Each describes main as it is, a PR that makes one wrong fixes it in the same PR, and nothing is written twice: a doc
points at the other.

- `README.md`, for **humans**: what it is, the enhancements and their settings, building, controls, music, performance,
  mods. Plain words, no internals. It changes when a player would notice: an enhancement added, removed or renamed (the
  list *and* the settings table), a default, the status line, build or run steps, where settings, saves, music or mods
  live, default bindings, mod support. Keep the note at the top saying that this is made with AI.
- **Comments beside the code**, for how and why: each `src/` file's header, the comment on every hook in `us.toml`,
  `tools/extract.py` (memory map), `gen_syms.py` (naming), `find_spins.py` (busy-wait loops),
  `runtime-patches/README.md` (each patch). Read the header of the file you change first and update it in the same
  commit. A finding that explains code goes beside that code.
- `docs/`, read when the task needs it:
  - `docs/building.md`: a fresh checkout, the remote build, running the game, the developer flags, testing in the game.
  - `docs/releasing.md`: desktop releases, publishing the page, moving `lib/N64Recomp-browser`.
  - `docs/web.md`: what of the web target is this game's own, and the `headless/` build.
  - `docs/ruled-out.md`: what has been tried and measured, so don't retry it. Read it before a performance or
    rendering change.
- **This file**: only what an agent can't get from the others. It changes when a rule changes.

This repo and the web repos are public. Write for a stranger, in the present tense: nothing about a contributor's own
machine, servers, paths or accounts, no person's name, no history of renames or "until <date>", and nothing about where
anyone keeps ROMs.

## Rules

- No ROMs, ROM-derived assets or extracted game data in git, CI, PRs or releases (`tools/ci/check_repo.py` checks).
  The generated hi-res font pack (`hires_font/`) and title pack (`hires_title/`) are ROM-derived too.
  `assets/title.patch` is allowed (an upscale described in terms of the game's own title); make a new one with
  `tools/title_patch.py`, never commit the picture.
- Every enhancement is an option with an Original value that runs the game as it shipped, on the stock tab for its
  topic (General, Graphics, Sound, Controls), with a line in the README's list and a row in its settings table.
- Function and variable names in `syms/` are the mod API: add names, never rename or remove one.
- The game's changes to the submodules go in `runtime-patches/` (applied by `tools/apply_runtime_patches.sh`), never
  committed inside `lib/`. A new change is a new patch file per submodule, numbered after that submodule's highest,
  with a line in `runtime-patches/README.md`; check that the set still applies to a clean checkout of the submodule
  with N64Recomp-browser's patch in.
- Never open a PR, issue or discussion upstream at rt64, plume, N64Recomp, N64ModernRuntime, RecompFrontend,
  BanjoRecomp, Zelda64Recomp or Donkey-Kong-64-Recompiled: their CONTRIBUTING.md asks for no LLM-written code
  (plume's, no AI-made contribution and no AI talking to its maintainers). The web forks stay forks.
- Code from a repo without a license is never copied: facts learned from one only.
- Priority is by what it breaks: high for a crash, high or med for a GitHub issue (high when it stops someone
  playing), low for a small improvement. Work without one gets one from whoever first can tell (often its title
  says), before anything else they do on it; a priority found wrong is changed at once, in the same work. This recomp
  is the project's own, so its work comes before N64Recomp-browser's of the same kind.
- CI (`.github/workflows/ci.yml`) has no ROM: it builds everything but the recompiled code and stops short of linking.
  Anything that changes behaviour needs a run of the game before it lands (`docs/building.md`). Pure code (resampler,
  limiters, the MusyX side mix, Draw Distance's matrix scaling) has tests in `tests/` that CI runs; new pure code gets
  one.
- A merge that changes what the desktop game runs is shipped by the agent whose merge it is, in the same work and
  without being asked: a desktop release (Windows and Linux), one release carrying every merge since the last, and, if
  it changes what the page is built from, the page played and republished. `docs/releasing.md` says which files count,
  how a change's line waits in `releases/next.md` and who cuts the release.

## How it fits together

- `tools/extract.py` writes `roms/rush2049.us.recomp.z64`, the ROM with its deflate-compressed main code and two
  overlays appended inflated at fake ROM addresses; `gen_syms.py` writes the symbol file; N64Recomp (`us.toml`) turns
  it into `RecompiledFuncs/` and RSPRecomp (`musyx.toml`) the audio ucode into `rsp/musyx.cpp` (`tools/recompile.sh`).
  Both are generated, not in git, and `iterate.sh` regenerates `RecompiledFuncs/`, so never edit them.
- `us.toml` is where game code is changed: `stubs`, `ignored`, instruction patches and `[[patches.hook]]` C text
  calling into `src/`. N64Recomp drops or reimplements libultra **by name**, so an unnamed libultra function that
  touches hardware is recompiled and crashes, and a game function wrongly given a libultra name silently disappears.
- The runtime registers code only where the game's inflate routine is hooked (`src/code_loading.cpp`). Game addresses
  map to `rdram + (addr - 0x80000000)`; a fault in guard memory minus the RDRAM base is the hardware register touched.
- ultramodern runs one game thread at a time and delivers interrupts only inside OS calls, so a loop polling memory
  never ends unless it yields: `find_spins.py --toml` generates those hooks into `us.toml`.
- Mods: the patches library (`patches/`, MIPS C built with the MIPS clang) is what mods import from `"*"`, with
  natives in `src/mod_exports.cpp`; `decompression_routine` rebuilds the recomp ROM so the runtime can recompile hooked
  functions.
- The `rt64`, `N64ModernRuntime` and `RecompFrontend` submodules are upstream's, at the commits N64Recomp-browser's
  `patches/patches.txt` names (CI's checks job compares them). `tools/apply_runtime_patches.sh` applies
  N64Recomp-browser's patches (each fork as one patch on upstream) and then `runtime-patches/`, so the tree is the
  forks' with the game's changes, natively too. A fork change is named by its label in N64Recomp-browser's
  `docs/forks.md` (rt64-0007) or by its fork and commit (rt64 7734fe4).
- The desktop build sets three of the forks' options that are on with Emscripten (CMakeLists.txt):
  `RT64_FLIPPED_RECT_HIGH_RES` (rt64 fe91344, the hi-res title's strips), `ULTRAMODERN_LIBULTRA_THREAD_STOP`
  (N64ModernRuntime 0df8334: the game stops its threads from other threads) and
  `RECOMPINPUT_SINGLE_PLAYER_UNTIL_ASSIGNED` (RecompFrontend bf01590: Per Player is One Player until a controller
  joins, and a keyboard player starts from its keys).
