#!/usr/bin/env bash
# Patches the submodules under lib: each project gets the web support first, N64Recomp-browser's
# patches/<name>.patch, then this game's own changes, runtime-patches/<name>-*.patch in order of name. The projects
# are the rows of N64Recomp-browser's patches/patches.txt, which says where each one is (rt64/src/contrib/plume,
# N64ModernRuntime/N64Recomp), and the submodules pin the upstream commits named there, the ones those patches were
# made on (CLAUDE.md, "How it fits together"). Patches already applied are skipped, and so is a project that isn't checked
# out, and so is a game patch that N64Recomp-browser's has taken in (its fork made the same change), until the pin
# moves onto it and the patch is deleted. Run after cloning or updating submodules, and again after pulling new patches.
# Which patches are applied is found on a scratch copy of the files they touch: they are reversed there newest first,
# so a patch whose lines a later one changed reverses once that one is off, and each one that reverses was applied.
# A checkout can hold a later patch without an earlier one, if a patch was added or edited on main after the others
# were applied. Then the applied ones after the first missing one come off and everything from it goes
# on in order, so the tree is the same as a fresh checkout's. The whole set is tried on the copy first, and a patch
# that doesn't apply (the submodule has other changes, or an applied patch was since edited, as N64Recomp-browser's
# are when it moves) stops the script before the submodule is touched.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
browser="$root/lib/N64Recomp-browser/patches"
[[ -f "$browser/patches.txt" ]] || { echo "lib/N64Recomp-browser isn't checked out" >&2; exit 1; }
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
export GIT_CEILING_DIRECTORIES="$scratch"  # git apply in a copy works on its files, never on a repo above it
subs=()
declare -A sub_path sub_patches
while read -r sub path _; do
    subs+=("$sub")
    sub_path[$sub]="$path"
    sub_patches[$sub]="$browser/$sub.patch"$'\n'
done < <(grep -v '^#' "$browser/patches.txt" | grep .)
for patch in "$root"/runtime-patches/*.patch; do
    name="$(basename "$patch")"
    sub="${name%%-[0-9]*}"
    [[ -v sub_path[$sub] ]] || { echo "$name: no $sub in N64Recomp-browser's patches/patches.txt" >&2; exit 1; }
    sub_patches[$sub]+="$patch"$'\n'
done
for sub in "${subs[@]}"; do
    mapfile -t list < <(printf %s "${sub_patches[$sub]}")
    dir="$root/lib/${sub_path[$sub]}"
    if [[ ! -e "$dir/.git" ]]; then  # an uninitialised submodule is an empty folder
        for patch in "${list[@]}"; do echo "not checked out, skipped: $(basename "$patch")"; done
        continue
    fi
    # A patch of the game's that N64Recomp-browser's patch has taken in (its fork made the same change) stops applying
    # after it, and is left out until the pin moves onto a release with it and the patch goes from runtime-patches/. It
    # is found on another scratch copy, of the files at the submodule's own commit: one that won't go on there after
    # the patches before it, and comes off instead, is already in them.
    base="$scratch/$sub.base"
    sed -n 's#^\(---\|+++\) [ab]/##p' "${list[@]}" | sort -u | while read -r path; do
        if git -C "$dir" cat-file -e "HEAD:$path" 2>/dev/null; then
            mkdir -p "$base/$(dirname "$path")" && git -C "$dir" show "HEAD:$path" > "$base/$path"
        fi
    done
    mkdir -p "$base"
    kept=("${list[0]}")
    (cd "$base" && git apply --whitespace=nowarn "${list[0]}" 2>/dev/null) || true
    for patch in "${list[@]:1}"; do
        if ! (cd "$base" && git apply --whitespace=nowarn "$patch" 2>/dev/null) &&
            (cd "$base" && git apply --whitespace=nowarn --reverse --check "$patch" 2>/dev/null); then
            echo "in N64Recomp-browser's $sub patch already, skipped: $(basename "$patch")"
        else
            kept+=("$patch")
        fi
    done
    list=("${kept[@]}")
    copy="$scratch/$sub"
    mkdir -p "$copy"
    sed -n 's#^\(---\|+++\) [ab]/##p' "${list[@]}" | sort -u | while read -r path; do
        if [[ -e "$dir/$path" ]]; then (cd "$dir" && cp --parents -p "$path" "$copy/"); fi
    done
    applied=()
    for ((k = ${#list[@]} - 1; k >= 0; k--)); do
        if (cd "$copy" && git apply --whitespace=nowarn --reverse "${list[$k]}" 2>/dev/null); then applied[$k]=1; fi
    done
    first=${#list[@]}
    for ((k = 0; k < ${#list[@]}; k++)); do
        if [[ -z "${applied[$k]:-}" ]]; then first=$k; break; fi
    done
    for ((k = 0; k < ${#list[@]}; k++)); do
        if ! out="$(cd "$copy" && git apply --whitespace=nowarn "${list[$k]}" 2>&1)"; then
            echo "$out" >&2
            echo "$(basename "${list[$k]}") doesn't apply to ${dir#"$root"/}: it has changes of its own, or a patch" \
                "applied there was edited since. Nothing was changed; a clean checkout of it takes every patch:" \
                "git -C ${dir#"$root"/} reset --hard && git -C ${dir#"$root"/} clean -fd, then this again." >&2
            exit 1
        fi
    done
    for ((k = 0; k < first; k++)); do echo "already applied: $(basename "${list[$k]}")"; done
    for ((k = ${#list[@]} - 1; k > first; k--)); do
        if [[ -n "${applied[$k]:-}" ]]; then
            git -C "$dir" apply --whitespace=nowarn --reverse "${list[$k]}"
            echo "taken off, to go back on in order: $(basename "${list[$k]}")"
        fi
    done
    for ((k = first; k < ${#list[@]}; k++)); do
        git -C "$dir" apply --whitespace=nowarn "${list[$k]}"
        echo "applied: $(basename "${list[$k]}")"
    done
done
