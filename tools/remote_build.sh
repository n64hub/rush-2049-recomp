#!/usr/bin/env bash
# Builds on a Linux host over ssh ($BUILD_HOST, a host name ssh knows) instead of the machine you run it on.
# Everything runs in a Docker image of the build tools, tools/docker/Dockerfile, tagged with a hash of that file and
# built on the host the first time a run needs it, so a missing tool is fixed in the Dockerfile, not installed on the
# host by hand.
#
# Each checkout gets a copy of its own there, ~/rush2049/<name>/repo (name: the checkout's folder), made by cloning
# origin and kept between runs with its build dirs, so a second build compiles only what changed. One ccache,
# ~/rush2049/ccache, serves every copy, and hits across them because paths in a checkout are relative to it
# (N64Recomp-browser's frontend/build_speed.cmake): a fresh copy of the game's page builds in ~80 s on 12 cores. A
# container is capped at $BUILD_CPUS cores (12) and 24 GB, so it leaves room for whatever else the host runs. Runs on
# one copy wait for each other (flock), and its container is named rush2049-<name>, so `docker stop` names it.
#
# Usage: BUILD_HOST=<host> tools/remote_build.sh [-C <checkout>] [-n <name>] <command>
#   sync [<commit>]   send <commit> (default HEAD, with the uncommitted changes to tracked files; untracked files are
#                     not sent) to the copy, check it out there with its submodules, patch them
#                     (tools/apply_runtime_patches.sh: N64Recomp-browser's patches/, then runtime-patches/), and copy
#                     roms/*.z64 when they differ. A commit on origin is fetched there, not sent.
#   run "<command>"   run a bash command in the image, in the copy's root, the copy's own HOME (the rest of the
#                     host's home isn't mounted)
#   build [<target>]  regenerate the recompiled code (tools/recompile.sh) and build <target> (Rush2049Recompiled, the
#                     game's page) in build-web, at -O3 unless the configure says otherwise:
#                     tools/remote_build.sh run "cmake build-web -DN64RECOMP_BROWSER_FAST_LINK=ON" for a quick try
#   web [<target>]    sync, then build
#   get <dir> <local dir>   copy the files at the top of <dir> (relative to the copy) here, build files left out
#   clean             delete the copy (ccache stays)
# -C works on another checkout, a game fork for one (only sync, run, get and clean mean anything there); -n names the
# copy, for one shared by several runs, as tools/publish_page.sh does; -d runs in another image, the Dockerfile given
# (tools/release.sh's Linux build, tools/docker/Dockerfile.linux).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
checkout="$(cd "$here/.." && pwd)"
name=""
dockerfile="$here/docker/Dockerfile"
while [[ $# -gt 0 ]]; do
    case "$1" in
    -C) checkout="$(cd "$2" && pwd)"; shift 2 ;;
    -n) name="$2"; shift 2 ;;
    -d) dockerfile="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    *) break ;;
    esac
done
cmd="${1:-}"
[[ -n "$cmd" ]] && shift
name="${name:-$(basename "$checkout")}"
base=rush2049                     # under the host's home
dir="$base/$name"
image="n64recomp-build:$(git hash-object "$dockerfile" | cut -c1-12)"
cpus="${BUILD_CPUS:-12}"
host="${BUILD_HOST:-}"

remote() { ssh -o BatchMode=yes "$host" "$@"; }

# The image, built on the host when it isn't there. The Dockerfile goes in on stdin: a build needs no context.
ensure_image() {
    remote "docker image inspect $image > /dev/null 2>&1 || { echo 'building $image on $host (once, ~10 min)'; \
        docker build -q -t $image - > /dev/null; }" < "$dockerfile"
}

# Runs a bash command in the image on the copy, holding its lock. The base dir is mounted at the same path inside, so
# paths in logs and caches read the same in and out of the container.
in_image() {
    ensure_image
    remote "set -e; cd ~/$dir; exec flock .lock docker run --rm --name $base-$name --cpus $cpus --memory 24g \
        -u \$(id -u):\$(id -g) -v \$HOME/$base:\$HOME/$base -w \$HOME/$dir/repo -e HOME=\$HOME/$dir/home \
        -e CCACHE_DIR=\$HOME/$base/ccache $image bash -c $(printf %q "$1")"
}

sync() {
    local commit="${1:-HEAD}" sha url diff=""
    sha="$(git -C "$checkout" rev-parse "$commit^{commit}")"
    url="$(git -C "$checkout" remote get-url origin)"
    remote "mkdir -p ~/$base/ccache ~/$dir/home && chmod 700 ~/$base && cd ~/$dir && \
        { [ -d repo/.git ] || git clone -q $url repo; }"
    # The commit and what it stands on that origin's main hasn't, as a bundle (none if origin has it all).
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    git -C "$checkout" update-ref refs/remote-build-sync "$sha"
    if git -C "$checkout" bundle create -q "$tmp/head.bundle" refs/remote-build-sync --not origin/main 2> /dev/null; then
        scp -q "$tmp/head.bundle" "$host:$dir/head.bundle"
    fi
    git -C "$checkout" update-ref -d refs/remote-build-sync
    if [[ "$commit" == HEAD ]]; then
        git -C "$checkout" diff HEAD --binary --ignore-submodules > "$tmp/work.diff"
        [[ -s "$tmp/work.diff" ]] && scp -q "$tmp/work.diff" "$host:$dir/work.diff" && diff=1
    fi
    remote "set -e; cd ~/$dir; exec flock .lock bash -s" << EOF
set -e
# No fetch recurses into submodules: the submodule update below fetches what the checkout names. A recursing fetch
# walks every branch new to a submodule for the commits its own submodules name, and rt64's old branches name plume
# and spirv-cross commits GitHub no longer serves ("not our ref"), so the first fetch after a pin moved failed.
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=fetch.recurseSubmodules GIT_CONFIG_VALUE_0=false
cd repo
git fetch -q origin
[ -f ../head.bundle ] && git fetch -q ../head.bundle refs/remote-build-sync && rm ../head.bundle
git checkout -q -f --detach $sha
git clean -fdq
[ -n "$diff" ] && git apply ../work.diff && rm ../work.diff
# Submodules: the ones whose commit moved are reset (patches off) and updated; nested ones move once their parents
# have, hence twice. One that didn't move keeps its files and their times, so ninja rebuilds nothing there.
moved=\$(git submodule status | sed -n 's/^[-+][0-9a-f]* \([^ ]*\).*/\1/p')
for sub in \$moved; do
    if [ -e "\$sub/.git" ]; then
        git -C "\$sub" submodule foreach --recursive -q 'git reset -q --hard && git clean -fdq'
        git -C "\$sub" reset -q --hard && git -C "\$sub" clean -fdq
    fi
done
# The host has no GitHub key, so a public repo's submodule on a git@github.com: URL (a game fork's decomp can have
# one) is fetched over HTTPS.
for i in 1 2; do git submodule sync -q --recursive
    git -c url.https://github.com/.insteadOf=git@github.com: submodule update -q --init --recursive; done
if [ -f tools/apply_runtime_patches.sh ]; then
    # A patch main has since edited (N64Recomp-browser's, when it moved) makes the script stop before it touches
    # anything: then all of them start over.
    bash tools/apply_runtime_patches.sh > /dev/null 2>&1 || {
        git submodule foreach --recursive -q 'git reset -q --hard && git clean -fdq'
        bash tools/apply_runtime_patches.sh > /dev/null; }
fi
git log --oneline -1
EOF
    # The ROMs (N64Recomp reads the recomp one, RSPRecomp the plain one), each copied when it differs from the host's.
    local rom have
    have="$(remote "mkdir -p ~/$dir/repo/roms && cd ~/$dir/repo && sha1sum roms/*.z64 2> /dev/null || true")"
    for rom in "$checkout"/roms/*.z64; do
        [[ -f "$rom" ]] || continue
        if ! grep -qx "$(sha1sum < "$rom" | cut -c1-40)  roms/$(basename "$rom")" <<< "$have"; then
            scp -q "$rom" "$host:$dir/repo/roms/"
        fi
    done
}

[[ -n "$host" || -z "$cmd" ]] || { echo "set BUILD_HOST to the ssh host to build on"; exit 1; }
case "$cmd" in
sync) sync "$@" ;;
run) in_image "$1" ;;
build | web)
    [[ "$cmd" == web ]] && sync
    in_image "set -e
        bash tools/recompile.sh -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        # The ports CMake finds and ninja depends on, in the image's own cache, which goes with the container (seconds).
        embuilder build sdl2-mt freetype-legacysjlj > /dev/null 2>&1
        [ -f build-web/build.ninja ] || emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release > build-web.log 2>&1 ||
            { tail -20 build-web.log; rm -f build-web/build.ninja; exit 1; }
        ninja -C build-web -j $cpus ${1:-Rush2049Recompiled} > build-web/build.log 2>&1 ||
            { grep -E ' error|FAILED|undefined' build-web/build.log | head -20; exit 1; }
        echo built"
    ;;
get)
    mkdir -p "$2"
    remote "cd ~/$dir/repo/$1 && find . -maxdepth 1 -type f ! -name '*.ninja*' ! -name '*.log' ! -name '*.a' ! -name '*.json' ! -name CMakeCache.txt \
        ! -name '*.cmake' ! -name '.*' | tar cf - -T -" | tar xf - -C "$2"
    ;;
clean) remote "cd ~/$base && flock $name/.lock rm -rf $name" ;;
*) sed -n 2,26p "$0"; exit 1 ;;
esac
