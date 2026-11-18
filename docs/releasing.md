# Releasing

Shipping a change means both: the page and a desktop release, each by the agent whose merge changed it, as part of the
same work and without being asked, so a fix never reaches only one of them.

- A merge that changes what the desktop game runs (the files the page is built from, below, less code only the
  Emscripten build compiles) gets a desktop release, Windows and Linux: one release carries every such merge since the
  last (below).
- A merge that changes what the page is built from gets the page played and republished: `src/`, `us.toml`,
  `patches/`, `assets/`, `CMakeLists.txt`, `runtime-patches/`, a submodule or `lib/N64Recomp-browser` pin that changes
  the page's code, the prebuilt shader list. Not docs, tests, CI or a tool the build doesn't run.
- Code only the web build compiles needs no release, and code only the desktop build compiles republishes no page.

## A desktop release

One release carries every change merged since the last. A change's PR adds what the player gets to `releases/next.md`, a
line under the others (the first change after a release makes the file), and bumps no version. Once it merges, the work
stays until a release that carries it is out: with none in flight it releases, unasked; with one in flight it waits for
that one and looks again, since a release started meanwhile may carry it. A release is in flight from the moment its PR
is opened until GitHub has the release (`gh pr list --search "Release v in:title"`,
`gh release view v<main's version_string>`), and one left in flight by work that stopped is finished by whoever finds
it. A change merged during one rides the next, and nobody holds a merge for one.

A release is cut on a machine with the ROM, never in CI (no ROM, so no link). Its PR, titled `Release v<version>`, bumps
`version_string` in `src/main.cpp` and moves `releases/next.md` to `releases/v<version>.md` (`git mv`, then the lines
written into the last release's shape). Once it merges, from a clean checkout of that merge commit with a full setup
(`docs/building.md`): `BUILD_HOST=<host> BOOT_RELEASE=<script> bash tools/release.sh` (`--dry-run` builds and packs
only). It builds the Windows zip and, on the build host, the Linux `.tar.gz`, boots each from a new folder (the script
`BOOT_RELEASE` names; look at the captures in `out/boot-windows` and `out/boot-linux`), and publishes GitHub release
`v<version>` with both and that file as its notes, refusing without one, with `releases/next.md` still there or at a
commit not on `origin/main`. Its header says what each holds and how the Linux build boots: under Mesa's software
Vulkan, which crashes on its own as the second attract race begins; no Linux GPU has run it.

The notes are for someone downloading it: what changed in the desktop game since the last release, in the README's
words, never the list of merged PRs (most are web support and other games' pages); `releases/v0.1.0.md` is the shape.

## Moving `lib/N64Recomp-browser`

A web change lands and is tagged in N64Recomp-browser and reaches this game when a pull request here moves the pin,
which comes from one of three places:

- A critical fix for the page on Pages (it crashes, never boots, loses its WebGPU device or can't be played) moves it at
  once, its page played in both browsers first.
- A runtime patch here that stops applying over N64Recomp-browser's newest tag (its CI's `rush2049-patches` job, red on
  every code PR there) moves it at once, in the work that finds it: the patch is made again over that tag in the same
  PR. A change here to a runtime patch that only fits the pin, as one that edits a line the newer patches changed does,
  is the usual cause.
- A tag whose change a player would notice in this game (a crash, hang or stuck game fixed, sound, a feature, a
  desktop-build fix, a speed-up they would see) moves it once the tag is out, in the work that made the change or the
  work it was made for, as a player's desktop crash fixed in the renderer was.

Nothing else moves it. The pull request moves `lib/N64Recomp-browser` to the tag (and the submodules, if
`patches/patches.txt` names new upstream commits), resets each project whose patch changed and reruns
`tools/apply_runtime_patches.sh` (it stops on one and says how; rebase a game patch that stops applying in the same PR),
and builds and runs the desktop game. Once it has merged, whoever merged it ships it in the same work, and sees a
desktop release carry it when the tag changes native code (the generated `RecompiledFuncs/`, rt64, the runtime), as
above. A change only the desktop build compiles (rt64 7734fe4) is checked by a run of the desktop game and republishes
no page; a web-only change needs no desktop release.
