#ifndef __MODDING_H__
#define __MODDING_H__

// Section attributes the recomp tools read (RecompModTool for a mod, N64Recomp for the patches library). The same
// macros as in Zelda64Recomp's and BanjoRecomp's mod templates; do not change them.

#ifdef __cplusplus
#   define EXTERNC extern "C"
#else
#   define EXTERNC
#endif

// A function another mod exports, or the game exports with mod "*". noinline and weak let the definition sit in a
// header and keep the call for the tool to find.
#define RECOMP_IMPORT(mod, func) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"") \
    _Pragma("GCC diagnostic ignored \"-Wreturn-type\"") \
    EXTERNC __attribute__((noinline, weak, used, section(".recomp_import." mod))) func {} \
    _Pragma("GCC diagnostic pop")

// A function other mods can import from this one.
#define RECOMP_EXPORT EXTERNC __attribute__((retain, section(".recomp_export")))

// Replaces the game function of the same name. Without a decompilation that means rewriting it whole in C, so
// RECOMP_HOOK is usually what you want.
#define RECOMP_PATCH EXTERNC __attribute__((retain, section(".recomp_patch")))

#define RECOMP_FORCE_PATCH EXTERNC __attribute__((retain, section(".recomp_force_patch")))

// An event other mods can register callbacks for; call it to run them.
#define RECOMP_DECLARE_EVENT(func) \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"") \
    EXTERNC __attribute__((retain, noinline, weak, used, section(".recomp_event"))) void func {} \
    _Pragma("GCC diagnostic pop")

// Runs when `mod` (or "*" for the game) raises `event`.
#define RECOMP_CALLBACK(mod, event) __attribute__((retain, section(".recomp_callback." mod ":" #event)))

// Runs when the game function `func` is entered, with the same arguments, or just before it returns, with none (its
// argument registers hold whatever the function left there: keep what a return hook needs from an entry hook). `func`
// is a name from syms/rush2049.us.syms.toml.
#define RECOMP_HOOK(func) __attribute__((retain, section(".recomp_hook." func)))
#define RECOMP_HOOK_RETURN(func) __attribute__((retain, section(".recomp_hook_return." func)))

#endif
