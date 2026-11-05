# Source this from Git Bash: `. tools/env.sh`
# Puts the per-user N64 toolchain (installed under %LOCALAPPDATA%\Programs\n64-toolchain:
# clang-cl, CMake 3.31, Ninja, make, a portable MSVC and Windows SDK, a MIPS clang, ccache) on PATH and points clang-cl at
# the portable MSVC + Windows SDK. With ccache on PATH every build's compiles go through it (CMakeLists.txt, and for the
# web build lib/N64Recomp-browser/frontend/build_speed.cmake).
N64TC="${LOCALAPPDATA:-$HOME/AppData/Local}/Programs/n64-toolchain"
N64TC="$(cygpath -u "$N64TC")"
# Without it the environment stays as it is: a toolchain already on PATH (clang-cl, CMake, Ninja, make, the MSVC
# libraries in INCLUDE and LIB) with MIPS_CC and MIPS_LD set, such as a WSL clang: MIPS_CC="wsl clang" (patches/
# builds with paths relative to it, which WSL maps).
[ -d "$N64TC" ] || return 0
_msvc="$N64TC/portable-msvc/msvc"
_vc="$_msvc/VC/Tools/MSVC/14.44.35207"
_sdk="$_msvc/Windows Kits/10"
_sdkv="10.0.26100.0"

export PATH="$N64TC/llvm/bin:$N64TC/cmake/bin:$N64TC/ninja:$N64TC/ccache:$N64TC/make/bin:$_vc/bin/Hostx64/x64:$_sdk/bin/$_sdkv/x64:$PATH"
export INCLUDE="$(cygpath -w "$_vc/include");$(cygpath -w "$_sdk/Include/$_sdkv/ucrt");$(cygpath -w "$_sdk/Include/$_sdkv/shared");$(cygpath -w "$_sdk/Include/$_sdkv/um");$(cygpath -w "$_sdk/Include/$_sdkv/winrt");$(cygpath -w "$_sdk/Include/$_sdkv/cppwinrt")"
export LIB="$(cygpath -w "$_vc/lib/x64");$(cygpath -w "$_sdk/Lib/$_sdkv/ucrt/x64");$(cygpath -w "$_sdk/Lib/$_sdkv/um/x64")"

# The llvm.org Windows clang has no MIPS backend, so the MIPS patches build compiles with MSYS2's
# clang64 clang. llvm.org's ld.lld links MIPS ELF fine (MSYS2's lld picks the wrong driver).
# Pass these to make: `make CC="$MIPS_CC" LD="$MIPS_LD"`.
export MIPS_CC="$N64TC/msys64/clang64/bin/clang.exe"
export MIPS_LD="$N64TC/llvm/bin/ld.lld.exe"
export MIPS_OBJDUMP="$N64TC/msys64/clang64/bin/llvm-objdump.exe"
unset _msvc _vc _sdk _sdkv
