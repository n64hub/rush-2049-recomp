# The web target

The game plays in a browser (README): built with Emscripten on N64Recomp-browser's `frontend/` into
`Rush2049Recompiled.html` (the launcher, recompui's menus, SDL2's audio and keyboard, the ROM from Load ROM, and HOME
kept in IndexedDB; `N64Recomp-browser/frontend/CMakeLists.txt`'s header says what each part does), drawn through WebGPU.

What is the game's own stays here: `headless/`, the `EMSCRIPTEN` branches of `CMakeLists.txt` that make the game a
page, its prebuilt shader list (`src/rt64_shaders.txt`, recorded by `tools/shader_plan.json`; the list's header says
how to record it again and what its sections save) and its play plans (`tools/play_plan.json`, its regular play of
2-3 minutes, and `tools/play_plan_extended.json`, for when a longer play is needed). The rest is N64Recomp-browser's: `docs/porting.md` (WebGPU and the shaders, prebuilt shader lists, Emscripten, what the recompiled
code needed for wasm), `docs/playing.md` (playing and measuring a page), `docs/forks.md` (the fork changes that make
RT64 and recompui draw through WebGPU) and `docs/findings.md` (what is known about WebGPU in each browser). Every play of
this game's page is a line in its `plays.jsonl` (game `rush`), never a record here.

This repo's CI builds no Emscripten target: the page and `file.cpp`'s picker are checked only by a build and a run of
the game.

RT64's render to RAM (each frame drawn again at the N64's resolution and read back into RDRAM) is off by default, on the
desktop too: the Copy Frames to RAM option in `src/config.cpp` says why, `headless/main.cpp` what it costs.

Scenery Culling isn't offered in a browser, which keeps the game's culling (`src/config.cpp`): with it off, the page
ran out of its 1536 MB heap (N64Recomp-browser's `frontend/CMakeLists.txt`, `INITIAL_MEMORY`, never grown) as the
attract race handed over to the title screen. With the culling on, the heap already goes from about 700 MB in the race
to about 1180 MB at that hand-over; with it off it climbed about 300 MB a second from there. The desktop game holds
steady through it either way.

In a browser, Game Frame Rate's 60 holds the game to 30 while the device can't keep 60 up, and tries 60 again now and
then: on a phone that gave every frame two drawn, where 60 drew few between them. `src/frame_rate.cpp`'s header says
how and what it measured.

## headless/

`headless/` builds the recompiled code, N64ModernRuntime and the MusyX audio with no window, menus or input, natively
and with Emscripten. With Emscripten it draws through `N64Recomp-browser/renderer/` as a page, `Rush2049Headless.html`
(`HEADLESS_RT64`, on by default), or runs in node without it. Its `main.cpp` header says what it prints and what was
measured with it, native against node and the browser. Its link options in `headless/CMakeLists.txt` predate
`frontend/` and overlap it. Build it outside the repo: the `.data` file holds the ROM. On Windows, emsdk's `emsdk.bat`
and `emsdk_env.sh` can find a Python 2.7 first; set `EMSDK_PYTHON` to a Python 3:

```sh
git clone https://github.com/emscripten-core/emsdk && cd emsdk
EMSDK_PYTHON=<python 3> <python 3> emsdk.py install latest && <python 3> emsdk.py activate latest
source emsdk_env.sh                # then, with RecompiledFuncs/ and rsp/musyx.cpp generated:
cargo install naga-cli             # naga 30; on Windows the GNU toolchain needs MSYS2's clang64/bin on PATH for dlltool
emcmake cmake -S headless -B <dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEADLESS_ROM=<rom.z64> \
    -DRT64_NAGA_EXECUTABLE=<naga.exe>      # if naga isn't on PATH; -DHEADLESS_RT64=OFF for node
ninja -C <dir>
cd <dir> && MSYS_NO_PATHCONV=1 node Rush2049Headless.js /rom.z64 --seconds 120     # a HEADLESS_RT64=OFF build
```

Serve `<dir>` over http on localhost with `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` (threads need SharedArrayBuffer; on GitHub Pages that takes
coi-serviceworker) and open `Rush2049Headless.html?seconds=60&canvas=960x720` in Chrome (headless works with
`--headless=new --enable-unsafe-webgpu`). The query is the command line (`page_arguments.js`; `render-to-ram=1` turns
render to RAM on), the console gets what stderr would, and CDP `Page.captureScreenshot` of that page shows it.
