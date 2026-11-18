#!/usr/bin/env bash
# Build a release from a local ROM, for Windows and for Linux on $BUILD_HOST, and publish both as a GitHub release. CI has no ROM (none is ever uploaded), so it can't link the game; releases are cut on a machine with the
# ROM instead.
# Usage: BUILD_HOST=<host> BOOT_RELEASE=<script> tools/release.sh [--dry-run]
#   One release carries every change merged since the last (docs/releasing.md): each change's PR adds what the
#   player gets to releases/next.md, and a release is a PR titled "Release v<version>" that bumps version_string
#   in src/main.cpp and moves releases/next.md to releases/v<version>.md: what a player gets, not a list of merged
#   PRs (most are web support and other games' pages, which the zip doesn't have). The tag is v<version_string>.
#   Without that file it refuses, and with releases/next.md still there. Run it from that PR's merge commit,
#   checked out: it needs a clean tree at a commit on origin/main (a change merged meanwhile goes in the next
#   release and doesn't stop this one), the
#   extract.py's roms/ (README, docs/building.md) and gh signed in. --dry-run builds and packs but publishes nothing and
#   skips the checks on the tree and the tag.
# Both build through tools/remote_build.sh, in a copy named release-linux on $BUILD_HOST (which gets roms/ too), kept
# from one release to the next with its build dirs, so only what changed rebuilds. The Windows build is cross-built
# there (remote_build.sh's windows, in tools/docker/Dockerfile.windows, which says how). The Linux one is built
# in tools/docker/Dockerfile.linux, Ubuntu 22.04, so it needs glibc 2.35 or newer. It carries the libSDL2 it was built
# with and libatomic (not on every system) beside the binary (RUNPATH $ORIGIN), as the Windows zip carries SDL2.dll, and
# is a .tar.gz so the binary stays executable.
# Both archives are booted before anything is published, by the script $BOOT_RELEASE names (with its .Dockerfile beside
# it, for the Linux one): unpacked into a new folder with the ROM stored, run with --boot for 30 s, its window captured.
# The Windows one boots here (out/boot-windows), the Linux one on $BUILD_HOST in its copy, in Ubuntu 24.04 under
# Xvfb with Mesa's software Vulkan, lavapipe (out/boot-linux, fetched back).
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
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
dry_run=false
[ "${1:-}" = "--dry-run" ] && dry_run=true

version=$(sed -n 's/^const std::string version_string = "\(.*\)";$/\1/p' src/main.cpp)
[ -n "$version" ] || { echo "no version_string in src/main.cpp"; exit 1; }
tag="v$version"
notes="releases/$tag.md"
[ -s "$notes" ] || { echo "no $notes: write the release notes for players first"; exit 1; }
name="Rush2049Recompiled-$tag-windows-x64"
linux="Rush2049Recompiled-$tag-linux-x64"
[ -n "${BUILD_HOST:-}" ] || { echo "set BUILD_HOST to the Linux host to build the Linux release on"; exit 1; }
boot=${BOOT_RELEASE:-}
[ -n "$boot" ] && [ -f "$boot" ] && [ -f "${boot%.sh}.Dockerfile" ] || {
    echo "set BOOT_RELEASE to the script that boots a release archive, <archive> <rom> <game id> <dir> [args...],"
    echo "with the image it boots the Linux one in beside it (<script name>.Dockerfile)"
    exit 1
}

if ! $dry_run; then
    # Submodule worktrees are dirty with the runtime patches applied; their pinned commits still count.
    [ -z "$(git status --porcelain --ignore-submodules=dirty)" ] || { echo "the tree has uncommitted changes"; exit 1; }
    git fetch -q origin main
    git merge-base --is-ancestor HEAD origin/main || { echo "HEAD is not on origin/main"; exit 1; }
    [ ! -e releases/next.md ] || { echo "releases/next.md is still there: move it to $notes in the release's PR"; exit 1; }
    ! gh release view "$tag" > /dev/null 2>&1 || { echo "release $tag exists: bump version_string first"; exit 1; }
fi

# HEAD (with any uncommitted changes, for a dry run) goes to the copy; each build there regenerates the recompiled code
# from the committed symbols (tools/recompile.sh), so it matches this commit and not a dev build's.
rb() { bash tools/remote_build.sh -n release-linux "$@"; }
# Windows, in the copy's build/xwin-windows: Release's own flags plus debug info, which doesn't change the code. The .pdb
# stays in out/ beside the zip and is never published: it holds the folders of every object and library it was linked
# from, and with them the build host's account, which no compiler flag reaches. Without it a crash's trace in the
# player's rush2049.log gives each frame as the module and an offset into it (src/crash_handler.cpp), which this
# release's .pdb names: put it beside that release's exe as Rush2049Recompiled.pdb and look the offsets up. The flags
# are written -X, not /X, which Git Bash would turn into a path.
rb windows Rush2049Recompiled -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -Ob2 -DNDEBUG -Z7" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -Ob2 -DNDEBUG -Z7" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="-INCREMENTAL:NO -DEBUG -OPT:REF -OPT:ICF"
build=build-release-windows
rm -rf "$build"
rb get build/xwin-windows "$build"
ssh "$BUILD_HOST" cat rush2049/release-linux/repo/build/xwin-windows/Rush2049Recompiled.pdb > "$build/Rush2049Recompiled.pdb"

# The game finds assets/ and recompcontrollerdb.txt in its working directory, or in its own folder when started from
# another (src/main.cpp), so everything sits beside the exe.
stage="out/$name"
rm -rf "$stage" "out/$name.zip"
mkdir -p "$stage"
cp "$build"/{Rush2049Recompiled.exe,SDL2.dll,dxil.dll,dxcompiler.dll} recompcontrollerdb.txt COPYING "$stage"/
cp "$build/Rush2049Recompiled.pdb" "out/$name.pdb"
cp -r assets "$stage"/
# CMake's own zip, so packing needs nothing the build doesn't.
(cd out && cmake -E tar cf "$name.zip" --format=zip "$name")
echo "out/$name.zip ($(du -h "out/$name.zip" | cut -f1))"

# Linux, in the copy's build-release. Its sync (in windows, above) resets a submodule that moved and patches them
# again, tools/recompile.sh keeps the times of the files that come out the same, and paths in and out of the container
# read the same, so only what changed rebuilds.
rb -d tools/docker/Dockerfile.linux run "set -e
    bash tools/recompile.sh -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    rm -rf out
    cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \\
        -DPATCHES_C_COMPILER=clang -DPATCHES_LD=ld.lld -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > build-release.log 2>&1 ||
        { tail -20 build-release.log; exit 1; }
    ninja -C build-release Rush2049Recompiled > build-release/build.log 2>&1 ||
        { grep -E ' error|FAILED|undefined' build-release/build.log | head -20; exit 1; }
    mkdir -p out/$linux
    cp build-release/Rush2049Recompiled recompcontrollerdb.txt COPYING out/$linux/
    cp -L /usr/local/lib/libSDL2-2.0.so.0 /usr/lib/x86_64-linux-gnu/libatomic.so.1 out/$linux/
    patchelf --set-rpath '\$ORIGIN' out/$linux/Rush2049Recompiled
    cp -r assets out/$linux/
    tar -C out -czf out/$linux.tar.gz $linux"
rb get out out
echo "out/$linux.tar.gz ($(du -h "out/$linux.tar.gz" | cut -f1))"

bash "$boot" "out/$name.zip" roms/rush2049.us.z64 rush2049.n64.us out/boot-windows
# The Linux one in the copy, inside the image $BOOT_RELEASE boots it in (its .Dockerfile), which has no Docker, so the
# script boots it right there.
rb -d "${boot%.sh}.Dockerfile" run "mkdir -p ~/boot && cat > ~/boot/boot_release.sh << 'BOOT'
$(cat "$boot")
BOOT
    bash ~/boot/boot_release.sh out/$linux.tar.gz roms/rush2049.us.z64 rush2049.n64.us out/boot-linux"
rm -rf out/boot-linux
rb get out/boot-linux out/boot-linux

$dry_run && { echo "dry run: $tag not published"; exit 0; }
gh release create "$tag" "out/$name.zip" "out/$linux.tar.gz" --target "$(git rev-parse HEAD)" --title "Rush 2049 Recompiled $tag" \
    --notes-file "$notes"
