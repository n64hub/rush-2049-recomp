# Releasing and publishing

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

One release carries every change merged since the last, as N64Recomp-browser's CLAUDE.md (Releasing) has it for each
of the project's own recomps. A change's PR adds what the player gets to `releases/next.md`, a line under the others
(the first change after a release makes the file), and bumps no version. Once it merges, the work stays until a
release that carries it is out: with none in flight it releases, unasked; with one in flight it waits for that one and
looks again, since a release started meanwhile may carry it. A release is in flight from the moment its PR is opened
until GitHub has the release (`gh pr list --search "Release v in:title"`, `gh release view v<main's version_string>`),
and one left in flight by work that stopped is finished by whoever finds it. A change merged during one rides the
next, and nobody holds a merge for one.

A release is cut on a machine with the ROM, never in CI (no ROM, so no link). Its PR, titled `Release v<version>`,
bumps `version_string` in `src/main.cpp` and moves `releases/next.md` to `releases/v<version>.md` (`git mv`, then the
lines written into the last release's shape). Once it merges, from a clean checkout of that merge commit with a full
setup (`docs/building.md`): `BUILD_HOST=<host> bash tools/release.sh` (`--dry-run` builds and packs only). It builds the
Windows zip and, on the build host, the Linux `.tar.gz`, boots each from a new folder (N64Recomp-browser's
`tools/boot_release.sh`; look at the captures in `out/boot-windows` and `out/boot-linux`), and publishes GitHub release
`v<version>` with both and that file as its notes, refusing without one, with `releases/next.md` still there or at a
commit not on `origin/main`. Its header says what each holds and how the Linux build boots: under Mesa's software
Vulkan, which crashes on its own as the second attract race begins; no Linux GPU has run it.

The notes are for someone downloading it: what changed in the desktop game since the last release, in the README's
words, never the list of merged PRs (most are web support and other games' pages); `releases/v0.1.0.md` is the shape.

## The page

https://n64hub.com/rush-2049/ is published like a release, and no CI job does it. It is played before
it is published and booted from Pages after:

1. Build origin/main with `bash tools/publish_page.sh --dry-run` (from a checkout with `roms/`; it stages `out/page`).
2. Serve `out/page` with the README's headers and play it in Chrome and Firefox with N64Recomp-browser's
   `tools/play_page.mjs` and this repo's `tools/play_plan.json` (N64Recomp-browser's `docs/playing.md`). A build that
   plays worse than the runs recorded before it (`play_page.mjs` flags a regression, exit code 3) is not published;
   fixing it is the work.
3. Publish it (`bash tools/publish_page.sh`, or N64Recomp-browser's `tools/publish_pages.sh <checkout> rush`, which also
   boots it from Pages in headless Chrome) and boot it from Pages.
4. Record each run in N64Recomp-browser's `plays.jsonl` (`play_page.mjs --record plays.jsonl --game rush`) and merge
   the lines there in a pull request that changes only that file, even when this repo's pull request has already
   merged.

Both scripts build origin/main through `tools/remote_build.sh`, in a copy named `publish` (kept with its `build-web/`,
so a republish rebuilds only what changed and the `--dry-run` build is the one published), and publish the page into
the folder `rush-2049/` of the hub (n64hub/n64hub.github.io's `gh-pages`) with N64Recomp-browser's
`tools/publish_page.sh --into`. The hub's last commit names, a line each, the main each folder was built from
(`rush-2049/: built from main <sha>`); a page found behind main is played and published by whoever finds it. The first visit
reloads once (`coi_serviceworker.js`); right after a publish that can take a second visit.

## Moving `lib/N64Recomp-browser`

A web change lands and is tagged in N64Recomp-browser and reaches this game when a pull request here moves the pin,
which comes from one of two places (N64Recomp-browser's CLAUDE.md, The pins, and its `docs/publishing.md`):

- Its scheduled retest moves it to its newest tag, every change since the pin at once, having built this game's main
  with the pin at the tag (`tools/publish_pages.sh <checkout> --browser <tag>`) and played that build in Chrome and
  Firefox, so the page the pull request publishes is bytes that have already played. It also moves it to a tag that
  left the page's bytes alone, since the desktop build can still take something from it.
- A critical fix for the page on Pages (it crashes, never boots, loses its WebGPU device or can't be played) moves it at
  once, its page played in both browsers first.

Nothing else moves it. The pull request moves `lib/N64Recomp-browser` to the tag (and the submodules, if
`patches/patches.txt` names new upstream commits), resets each project whose patch changed and reruns
`tools/apply_runtime_patches.sh` (it stops on one and says how; rebase a game patch that stops applying in the same
PR), and builds and runs the desktop game. Once it has merged, whoever merged it ships it in the same work: publishes
the page and records its runs (above), and sees a desktop release carry it when the tag changes native code (the generated
`RecompiledFuncs/`, rt64, the runtime), as above, never leaving either to a card of its own. The pin's PR is the tree
the retest built, so it adds nothing to `releases/next.md`: the release the run cuts says what the move brings. A change only the desktop build
compiles (rt64 7734fe4) is checked by a run of the desktop game and republishes no page; a web-only change needs no
desktop release.
