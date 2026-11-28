#!/usr/bin/env bash
# Builds the desktop program's release, for Windows and for Linux, and publishes both as one GitHub release. CI has no
# ROM (none is ever uploaded), so it can't link the game; a release is built where the ROM is, from extract.py's
# roms/ (README, docs/building.md), and holds nothing from it: the player picks theirs in the launcher.
#
#     tools/release.sh windows   # Git Bash on Windows, with tools/env.sh's toolchain: out/<name>-windows-x64.zip
#     tools/release.sh linux     # Linux with Docker: out/<name>-linux-x64.tar.gz
#     tools/release.sh boot windows   # Git Bash on Windows: boots that archive from a new folder, out/boot-windows
#     tools/release.sh boot linux     # Linux with Docker: the same for the Linux archive, out/boot-linux
#     tools/release.sh publish   # both archives, built from this commit: the GitHub release (gh signed in)
#
# One release carries every change merged since the last (docs/releasing.md): each change's PR adds what the player
# gets to releases/next.md, and a release is a PR titled "Release v<version>" that bumps version_string in src/main.cpp
# and moves releases/next.md to releases/v<version>.md: what a player gets, not a list of merged PRs (most are web
# support and other games' pages, which the zip doesn't have). The tag is v<version_string>. Both builds and the
# publish run on that PR's merge commit, checked out with a clean tree (the runtime patches applied to the submodules
# don't count): each build refuses a tree with changes and records the commit it was built from
# (out/<archive>.commit), and publish refuses archives from another one, a Linux archive that hasn't booted or a Windows
# zip whose boot doesn't match it (boot), a tree with changes, a commit not on origin/main or still holding
# releases/next.md, or a tag that exists. A change merged meanwhile goes in the next release and doesn't stop this one.
# Each build regenerates the recompiled code from the committed symbols (tools/recompile.sh), so it matches this commit
# and not a dev build's.
#
# Windows: Release's own flags plus debug info, which doesn't change the code, in build/release-windows, so the dev
# build in build keeps its RelWithDebInfo configuration. The .pdb goes to out/ beside the zip and is never published:
# it holds the folders of every object and library it was linked from, and with them the builder's account, which no
# compiler flag reaches. Without it a crash's trace in the player's rush2049.log gives each frame as the module and an
# offset into it (src/crash_handler.cpp), which this release's .pdb names: put it beside that release's exe as
# Rush2049Recompiled.pdb and look the offsets up. The flags are written -X, not /X, which Git Bash would turn into a
# path. The DLLs sit beside the exe as the build leaves them (SDL2, dxcompiler, dxil).
# Linux: in the image tools/docker/Dockerfile.linux, Ubuntu 22.04, so the binary needs glibc 2.35 or newer. It carries
# the libSDL2 it was built with and libatomic (not on every system) beside the binary (RUNPATH $ORIGIN), as the Windows
# zip carries SDL2.dll, and is a .tar.gz so the binary stays executable. GTK 3, FreeType and a Vulkan driver are the
# system's.
# Either build can come from elsewhere: with WINDOWS_BUILD naming a folder that holds a build of Rush2049Recompiled
# from this commit with the flags above, its DLLs and .pdb (a cross-build, say), windows packs that instead of
# building, and run inside a container of tools/docker/Dockerfile.linux's image (where there is no Docker), linux
# builds right there.
# Both hold assets/, recompcontrollerdb.txt, recomplauncher.json and COPYING beside the program: the game finds them in
# its working directory, or in its own folder when started from another (src/main.cpp).
#
# boot runs $BOOT_RELEASE on the archive, which unpacks it into a new folder with the ROM stored, runs it with --boot
# (and any arguments after the OS) with its own window captured, and writes the <archive>.booted publish asks for.
# Publish needs the Linux archive's, and the Windows zip's too when lib/N64Recomp-browser's pin moved since the last
# release (or there is none yet): a change to the web support can break a Windows build alone (a name Windows' headers
# define), which the Linux boot doesn't see. Otherwise the zip's boot is optional, but a .booted beside the zip must
# match it.
# On lavapipe Ubuntu 24.04 and Arch boot it, find the ROM and draw the title and the first attract race, and then
# lavapipe (Mesa 25, 26.2) crashes on one of its own llvmpipe threads as the second race begins, about 50 s in. That is
# lavapipe's: it is the first draw with one of RT64's specialized pixel shaders (two LOD tiles, S mirrored), whose
# SPIR-V passes spirv-val with every texture index NonUniform, and whose code compiled by lavapipe holds the texture
# set's address in some lanes and zero in others; the validation layers report nothing, and with the ubershader alone it
# plays on. Debian 12's lavapipe (Mesa 22.3) has no descriptor indexing, so RT64 binds textures per draw there
# (rt64-0007, perDrawTextures) with the shaders built for it (rt64 7734fe4). With the 8192-texture array's shaders in
# that layout, lavapipe's lowering set a bit for each of the 8192 in a 128-bit set (textures_used) and overwrote the
# shader, crashing at boot. It boots, but slowly: black for about 60 s, then the attract race's first frame held for
# another 60 to 80 s while lavapipe's queue thread JIT-compiles shader variants with its LLVM 15 (the game waits on the
# RDP meanwhile; nothing is stuck), then the attract races at a few frames a second, so give it 4 minutes, and it plays
# the attract loop on until stopped. No Linux GPU has run it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

version=$(sed -n 's/^const std::string version_string = "\(.*\)";$/\1/p' src/main.cpp)
[ -n "$version" ] || { echo "no version_string in src/main.cpp" >&2; exit 1; }
tag="v$version"
name="Rush2049Recompiled-$tag"
rom=roms/rush2049.us.z64
commit=$(git rev-parse HEAD)
mkdir -p out

clean() {
    [ -z "$(git status --porcelain --ignore-submodules=dirty)" ] || { echo "the tree has uncommitted changes" >&2; exit 1; }
}

setup() {
    [ -f "$rom" ] || { echo "no $rom (README, docs/building.md)" >&2; exit 1; }
    clean
}

case "${1:-}" in
windows)
    if [ -n "${WINDOWS_BUILD:-}" ]; then
        build=$WINDOWS_BUILD
        [ -f "$build/Rush2049Recompiled.exe" ] && [ -f "$build/Rush2049Recompiled.pdb" ] ||
            { echo "no $build/Rush2049Recompiled.exe and .pdb" >&2; exit 1; }
        clean
    else
        setup
        source tools/env.sh
        bash tools/recompile.sh -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
        build=build/release-windows
        mkdir -p build
        cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
            -DCMAKE_C_FLAGS_RELEASE="-O2 -Ob2 -DNDEBUG -Z7" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -Ob2 -DNDEBUG -Z7" \
            -DCMAKE_EXE_LINKER_FLAGS_RELEASE="-INCREMENTAL:NO -DEBUG -OPT:REF -OPT:ICF" \
            -DPATCHES_C_COMPILER="${MIPS_CC:-clang}" -DPATCHES_LD="${MIPS_LD:-ld.lld}" > "$build.log" 2>&1 ||
            { tail -20 "$build.log"; exit 1; }
        ninja -C "$build" Rush2049Recompiled > "$build/build.log" 2>&1 ||
            { grep -E " error|FAILED|undefined" "$build/build.log" | head -20; exit 1; }
    fi
    stage="out/$name-windows-x64"
    rm -rf "$stage" "$stage.zip"
    mkdir -p "$stage"
    cp "$build"/{Rush2049Recompiled.exe,SDL2.dll,dxil.dll,dxcompiler.dll} recompcontrollerdb.txt recomplauncher.json COPYING "$stage"/
    cp "$build/Rush2049Recompiled.pdb" "$stage.pdb"
    cp -r assets "$stage"/
    # CMake's own zip, so packing needs nothing the build doesn't.
    (cd out && cmake -E tar cf "$name-windows-x64.zip" --format=zip "$name-windows-x64")
    echo "$commit" > "$stage.zip.commit"
    echo "$stage.zip ($(du -h "$stage.zip" | cut -f1))"
    ;;
linux)
    # The build, in tools/docker/Dockerfile.linux's image: in this host's Docker, or right here when already in it.
    # build/release-linux is kept from one release to the next, and tools/recompile.sh keeps the times of the files
    # that come out the same, so only what changed rebuilds.
    linux_build="set -euo pipefail
        git config --global --add safe.directory '*'
        bash tools/recompile.sh -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        build=build/release-linux
        mkdir -p \$build
        cmake -S . -B \$build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
            -DPATCHES_C_COMPILER=clang -DPATCHES_LD=ld.lld -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > \$build.log 2>&1 || { tail -20 \$build.log; exit 1; }
        ninja -C \$build Rush2049Recompiled > \$build/build.log 2>&1 ||
            { grep -E ' error|FAILED|undefined' \$build/build.log | head -20; exit 1; }
        stage=out/$name-linux-x64
        rm -rf \$stage \$stage.tar.gz
        mkdir -p \$stage
        cp \$build/Rush2049Recompiled recompcontrollerdb.txt recomplauncher.json COPYING \$stage/
        cp -L /usr/local/lib/libSDL2-2.0.so.0 /usr/lib/x86_64-linux-gnu/libatomic.so.1 \$stage/
        patchelf --set-rpath '\$ORIGIN' \$stage/Rush2049Recompiled
        cp -r assets \$stage/
        tar -C out -czf \$stage.tar.gz $name-linux-x64"
    if [ -f /.dockerenv ] && ! command -v docker > /dev/null; then
        clean
        bash -c "$linux_build"
    else
        setup
        image=rush2049-linux:$(sha256sum tools/docker/Dockerfile.linux | cut -c1-12)
        mkdir -p build/home build/ccache
        docker image inspect "$image" > /dev/null 2>&1 || docker build -t "$image" -f tools/docker/Dockerfile.linux tools/docker
        docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src -e CCACHE_DIR=/src/build/ccache -e HOME=/src/build/home \
            "$image" bash -c "$linux_build"
    fi
    echo "$commit" > "out/$name-linux-x64.tar.gz.commit"
    echo "out/$name-linux-x64.tar.gz ($(du -h "out/$name-linux-x64.tar.gz" | cut -f1))"
    ;;
boot)
    case "${2:-}" in
    windows) archive="out/$name-windows-x64.zip" ;;
    linux) archive="out/$name-linux-x64.tar.gz" ;;
    *) echo "usage: tools/release.sh boot windows|linux [program args...]" >&2; exit 1 ;;
    esac
    boot=${BOOT_RELEASE:-}
    [ -n "$boot" ] && [ -f "$boot" ] || {
        echo "set BOOT_RELEASE to the script that boots a release archive: <archive> <rom> <game id> <dir> [args...]" >&2
        exit 1
    }
    bash "$boot" "$archive" "$rom" rush2049.n64.us "out/boot-$2" "${@:3}"
    ;;
publish)
    notes="releases/$tag.md"
    [ -s "$notes" ] || { echo "no $notes: write the release notes for players first" >&2; exit 1; }
    clean
    git fetch -q origin main
    git merge-base --is-ancestor "$commit" origin/main || { echo "HEAD is not on origin/main" >&2; exit 1; }
    [ ! -e releases/next.md ] || { echo "releases/next.md is still there: move it to $notes in the release's PR" >&2; exit 1; }
    ! gh release view "$tag" > /dev/null 2>&1 || { echo "release $tag exists: bump version_string first" >&2; exit 1; }
    # N64Recomp-browser's pin against the last release's: moved (or no release yet), the Windows zip has to boot too.
    last=$(git describe --tags --abbrev=0 --match 'v*' 2> /dev/null || true)
    browser=moved pin=lib/N64Recomp-browser
    [ -z "$last" ] || [ "$(git rev-parse "$last:$pin")" != "$(git rev-parse "HEAD:$pin")" ] || browser=same
    files=("out/$name-windows-x64.zip" "out/$name-linux-x64.tar.gz")
    for f in "${files[@]}"; do
        [ -f "$f" ] || { echo "no $f: run tools/release.sh windows and linux first" >&2; exit 1; }
        [ "$(cat "$f.commit" 2> /dev/null)" = "$commit" ] || { echo "$f wasn't built from $commit" >&2; exit 1; }
        if [ "$f" = "out/$name-windows-x64.zip" ] && [ ! -e "$f.booted" ]; then
            [ "$browser" = moved ] || continue  # its boot is optional unless N64Recomp-browser moved (above)
            echo "$f hasn't booted, and N64Recomp-browser moved since ${last:-no release}: boot it first" >&2
            exit 1
        fi
        [ "$(cat "$f.booted" 2> /dev/null)" = "$(sha256sum "$f" | cut -d' ' -f1)" ] ||
            { echo "$f hasn't booted as it is now: run tools/release.sh boot on its OS first" >&2; exit 1; }
    done
    gh release create "$tag" "${files[@]}" --target "$commit" --title "Rush 2049 Recompiled $tag" --notes-file "$notes"
    ;;
*)
    echo "usage: tools/release.sh windows|linux|boot <os>|publish" >&2
    exit 1
    ;;
esac
