// Prints a symbolized stack trace to stderr when the process crashes (Windows, and Linux with less detail), on Linux
// every thread's when the game is killed or loses its X server (a hung game force-closed), and
// writes stdout and stderr to a log file when nobody is watching them (start_log_file), so a player has the
// trace to send in.
//
// Recompiled functions are named after their N64 address (func_800A11E4, ovl1_func_8038A634, ...), or by
// syms/names.toml for the few we have worked out (draw_tex_rect, ...), so a trace
// of host frames points straight at the game code that crashed. The symbols are in the .pdb beside the exe, which a
// build from source has; the release zip leaves it out (tools/release.sh says why and where it is kept), so a
// player's trace prints each frame as the module and the offset into it, which that release's .pdb names later.

#include "crash_handler.h"
#include "debug_probe.h"

#include <cstdint>

namespace {

[[maybe_unused]] const char* state_name(uint16_t state) {
    switch (state) {
        case 0: return "stopped";
        case 1: return "queued";
        case 2: return "running";
        case 3: return "blocked";
        default: return "?";
    }
}

} // namespace

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>
#include <map>
#include <string>
#include <algorithm>
#include <vector>

#include "ultramodern/ultra64.h"

#pragma comment(lib, "Dbghelp.lib")

namespace {

std::mutex crash_mutex;

// Native thread id -> game thread, recorded by game_thread_name (ultramodern calls it on each new game thread).
std::mutex game_threads_mutex;
std::map<DWORD, const OSThread*> game_threads;

bool sym_initialized = false;

void init_symbols() {
    if (!sym_initialized) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        // The exe's own folder first, where a build puts the .pdb, whatever the working directory.
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path{ exe, n };
        path = path.substr(0, path.find_last_of(L"\\/") + 1) + L";.";
        SymInitializeW(GetCurrentProcess(), n > 0 && n < MAX_PATH ? path.c_str() : nullptr, TRUE);
        sym_initialized = true;
    }
}

// only_game: print just recompiled game frames and the libultra replacements they call, on one line.
void print_trace(CONTEXT* context, HANDLE thread = GetCurrentThread(), bool only_game = false) {
    HANDLE process = GetCurrentProcess();
    init_symbols();

    CONTEXT ctx = *context;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);

    for (int depth = 0; depth < 64; depth++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0) {
            break;
        }

        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        const char* name = SymFromAddr(process, pc, &displacement, symbol) ? symbol->Name : "?";
        // Only a .pdb's names count: without one dbghelp names an address after the nearest export below it, which
        // in a DLL is as often another function, so the frame is given as the module and offset instead. Asked
        // after SymFromAddr, which loads the module's symbols (they are deferred until then).
        IMAGEHLP_MODULE64 module_info{};
        module_info.SizeOfStruct = sizeof(module_info);
        if (!SymGetModuleInfo64(process, pc, &module_info) ||
            (module_info.SymType != SymPdb && module_info.SymType != SymDia)) {
            name = "?";
        }

        if (only_game) {
            if (strncmp(name, "func_", 5) == 0 || strstr(name, "_func_") != nullptr || strstr(name, "_recomp") != nullptr || strstr(name, "musyx") != nullptr ||
                (name[0] == '_' && name[1] == '_' && name[2] == 'o' && name[3] == 's')) {
                fprintf(stderr, " %s", name);
            }
            continue;
        }

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(process, pc, &line_displacement, &line)) {
            fprintf(stderr, "  #%02d %016llX %s+0x%llX (%s:%lu)\n", depth, (unsigned long long)pc, name,
                    (unsigned long long)displacement, line.FileName, line.LineNumber);
        }
        else if (name[0] != '?') {
            fprintf(stderr, "  #%02d %016llX %s+0x%llX\n", depth, (unsigned long long)pc, name,
                    (unsigned long long)displacement);
        }
        else {
            DWORD64 base = SymGetModuleBase64(process, pc);
            char module[MAX_PATH] = "?";
            if (base != 0) {
                GetModuleFileNameA(reinterpret_cast<HMODULE>(base), module, MAX_PATH);
            }
            const char* file = std::max(strrchr(module, '\\'), strrchr(module, '/'));
            fprintf(stderr, "  #%02d %016llX %s+0x%llX\n", depth, (unsigned long long)pc, file ? file + 1 : module,
                    (unsigned long long)(pc - base));
        }
    }
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    std::lock_guard lock{ crash_mutex };
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    fprintf(stderr, "\n[crash] exception 0x%08lX at %p", rec->ExceptionCode, rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        fprintf(stderr, " (%s 0x%016llX)", rec->ExceptionInformation[0] == 1 ? "write" : "read",
                (unsigned long long)rec->ExceptionInformation[1]);
    }
    fprintf(stderr, " on thread %lu", GetCurrentThreadId());
    {
        std::lock_guard threads_lock{ game_threads_mutex };
        auto it = game_threads.find(GetCurrentThreadId());
        if (it != game_threads.end()) {
            fprintf(stderr, " (game thread %d)", it->second->id);
        }
    }
    fprintf(stderr, "\n");
    print_trace(info->ContextRecord);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() (std::terminate's too) ends the process without an exception, so the filter above never sees it.
void abort_signal(int) {
    std::lock_guard lock{ crash_mutex };
    fprintf(stderr, "\n[crash] abort on thread %lu\n", GetCurrentThreadId());
    CONTEXT context{};
    RtlCaptureContext(&context);
    print_trace(&context);
    fflush(stderr);
}

void dump_all_stacks() {
    std::lock_guard lock{ crash_mutex };
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    fprintf(stderr, "[stacks]\n");
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) {
            continue;
        }
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (t == nullptr) {
            continue;
        }
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t, &ctx)) {
                fprintf(stderr, "  %5lu:", te.th32ThreadID);
                {
                    std::lock_guard threads_lock{ game_threads_mutex };
                    auto it = game_threads.find(te.th32ThreadID);
                    if (it != game_threads.end()) {
                        const OSThread* gt = it->second;
                        fprintf(stderr, " [id %d pri %d %s%s]", gt->id, gt->priority, state_name(gt->state),
                                gt->queue == (PTR(PTR(OSThread)))-1 ? " in run queue" : "");
                    }
                }
                print_trace(&ctx, t, true);
                fprintf(stderr, "\n");
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    rush2049::print_probes();
    fflush(stderr);
}

} // namespace

std::string rush2049::game_thread_name(const OSThread* t) {
    {
        std::lock_guard lock{ game_threads_mutex };
        game_threads[GetCurrentThreadId()] = t;
    }
    return "Game Thread " + std::to_string(t->id);
}

void rush2049::install_crash_handler() {
    SetUnhandledExceptionFilter(crash_filter);
    signal(SIGABRT, abort_signal);
}

void rush2049::install_x_error_handler() {}

void rush2049::start_stack_sampler(int interval_seconds, bool stacks) {
    std::thread([interval_seconds, stacks]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
            if (stacks) {
                dump_all_stacks();
            }
            else {
                rush2049::print_probes();
                fflush(stderr);
            }
        }
    }).detach();
}

void rush2049::start_byte_watch(uint32_t vram) {
    std::thread([vram]() {
        uint8_t* rdram;
        while ((rdram = rush2049::probe_get_rdram()) == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        volatile uint8_t* byte = rdram + ((vram & 0x7FFFFFFF) ^ 3);
        uint8_t last = *byte;
        int reports = 0;
        while (reports < 6) {
            uint8_t now = *byte;
            if (now != last) {
                fprintf(stderr, "[watch] %08X: %02X -> %02X\n", vram, last, now);
                dump_all_stacks();
                last = now;
                reports++;
            }
        }
    }).detach();
}

// --hwwatch <vram>: a hardware data breakpoint (x86 debug register 0) on one byte of RDRAM, set on every thread,
// so reads are caught too. Each new host instruction that touches the byte is printed once with its game stack.
namespace {

uintptr_t hw_watch_addr = 0;
std::mutex hw_watch_mutex;
std::map<DWORD64, int> hw_watch_hits;
thread_local bool in_hw_watch_filter = false;

LONG WINAPI hw_watch_filter(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || (info->ContextRecord->Dr6 & 1) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    info->ContextRecord->Dr6 = 0;
    // The breakpoint stays armed while this runs, so reading the byte below traps again on this thread. Without
    // this the nested call relocks hw_watch_mutex, which throws out of the handler and kills the process silently.
    if (in_hw_watch_filter) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    in_hw_watch_filter = true;
    std::lock_guard lock{ hw_watch_mutex };
    DWORD64 rip = info->ContextRecord->Rip;
    if (hw_watch_hits[rip]++ == 0 && hw_watch_hits.size() < 64) {
        std::lock_guard crash_lock{ crash_mutex };
        uint8_t value = *reinterpret_cast<volatile uint8_t*>(hw_watch_addr);
        fprintf(stderr, "[hwwatch] value %02X, thread %lu:", value, GetCurrentThreadId());
        print_trace(info->ContextRecord, GetCurrentThread(), true);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    in_hw_watch_filter = false;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void arm_hw_watch() {
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) {
            continue;
        }
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (t == nullptr) {
            continue;
        }
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx) && ctx.Dr0 != hw_watch_addr) {
                ctx.Dr0 = hw_watch_addr;
                ctx.Dr7 = (ctx.Dr7 & ~0xF0003ull) | 1 | (3ull << 16);  // local enable, read/write, 1 byte
                SetThreadContext(t, &ctx);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
}

} // namespace

void rush2049::start_hw_watch(uint32_t vram) {
    AddVectoredExceptionHandler(1, hw_watch_filter);
    std::thread([vram]() {
        uint8_t* rdram;
        while ((rdram = rush2049::probe_get_rdram()) == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        hw_watch_addr = reinterpret_cast<uintptr_t>(rdram + ((vram & 0x7FFFFFFF) ^ 3));
        while (true) {
            arm_hw_watch();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
}

void rush2049::start_profiler(int after_seconds, int seconds) {
    std::thread([after_seconds, seconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(after_seconds));
        HANDLE process = GetCurrentProcess();
        DWORD self = GetCurrentThreadId();
        struct Sampled {
            HANDLE handle;
            ULONG64 cycles = 0;
            uint32_t samples = 0;
            std::map<DWORD64, uint32_t> self_pcs;      // leaf
            std::map<DWORD64, uint32_t> stack_pcs;     // every frame, once per sample
        };
        std::map<DWORD, Sampled> threads;
        init_symbols();
        // A thread stopped in a system call (Nt*, Zw*) is waiting, nearly always, so it isn't counted as running.
        std::map<DWORD64, bool> syscall_pcs;
        auto in_syscall = [&](DWORD64 pc) {
            auto [it, added] = syscall_pcs.try_emplace(pc, false);
            if (added) {
                alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
                SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = MAX_SYM_NAME;
                DWORD64 displacement = 0;
                it->second = SymFromAddr(process, pc, &displacement, sym) &&
                    (strncmp(sym->Name, "Nt", 2) == 0 || strncmp(sym->Name, "Zw", 2) == 0);
            }
            return it->second;
        };
        auto start = std::chrono::steady_clock::now();
        uint32_t ticks = 0;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
            // Listing the threads walks every thread on the system, so it's done once a second.
            if (ticks++ % 1000 == 0) {
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self && !threads.contains(te.th32ThreadID)) {
                        Sampled& s = threads[te.th32ThreadID];
                        s.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                        if (s.handle != nullptr) {
                            QueryThreadCycleTime(s.handle, &s.cycles);
                        }
                    }
                }
                CloseHandle(snap);
            }
            for (auto& [id, s] : threads) {
                if (s.handle == nullptr) {
                    continue;
                }
                // Sample only a thread that ran since the last tick, so waits don't count.
                ULONG64 cycles = 0;
                QueryThreadCycleTime(s.handle, &cycles);
                bool ran = cycles != s.cycles;
                s.cycles = cycles;
                if (!ran || SuspendThread(s.handle) == (DWORD)-1) {
                    continue;
                }
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(s.handle, &ctx) && !in_syscall(ctx.Rip)) {
                    s.samples++;
                    s.self_pcs[ctx.Rip]++;
                    STACKFRAME64 frame{};
                    frame.AddrPC.Offset = ctx.Rip;
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrFrame.Offset = ctx.Rbp;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    frame.AddrStack.Offset = ctx.Rsp;
                    frame.AddrStack.Mode = AddrModeFlat;
                    for (int depth = 0; depth < 32 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, s.handle, &frame, &ctx,
                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) && frame.AddrPC.Offset != 0; depth++) {
                        s.stack_pcs[frame.AddrPC.Offset]++;
                    }
                }
                ResumeThread(s.handle);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        // Symbolize by function, so the pcs inside one function add up.
        alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
        auto name_of = [&](DWORD64 pc) {
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, pc, &displacement, symbol)) {
                return std::string(symbol->Name);
            }
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            return SymGetModuleInfo64(process, pc, &module) ? std::string(module.ModuleName) + "!?" : std::string("?");
        };
        auto by_function = [&](const std::map<DWORD64, uint32_t>& pcs, bool once_per_sample) {
            std::map<std::string, uint32_t> counts;
            for (auto [pc, n] : pcs) {
                counts[name_of(pc)] += n;
            }
            std::vector<std::pair<uint32_t, std::string>> sorted;
            for (auto& [name, n] : counts) {
                sorted.push_back({ n, name });
            }
            std::sort(sorted.rbegin(), sorted.rend());
            return sorted;
        };
        std::vector<std::pair<uint32_t, DWORD>> order;
        for (auto& [id, s] : threads) {
            order.push_back({ s.samples, id });
        }
        std::sort(order.rbegin(), order.rend());
        std::lock_guard lock{ crash_mutex };
        fprintf(stderr, "[profile] %u ticks over %.1f s; per thread: share of ticks it was running, then its top functions"
                        " by self time and by time on the stack\n", ticks, secs);
        for (auto [samples, id] : order) {
            if (samples < ticks / 50) {
                break;
            }
            Sampled& s = threads[id];
            fprintf(stderr, "[profile] thread %lu: %.1f%%\n", id, 100.0 * samples / ticks);
            auto self = by_function(s.self_pcs, false);
            for (size_t i = 0; i < self.size() && i < 12; i++) {
                fprintf(stderr, "[profile]   self %5.1f%% %s\n", 100.0 * self[i].first / samples, self[i].second.c_str());
            }
            auto stack = by_function(s.stack_pcs, true);
            for (size_t i = 0; i < stack.size() && i < 25; i++) {
                fprintf(stderr, "[profile]   total %5.1f%% %s\n", 100.0 * stack[i].first / samples, stack[i].second.c_str());
            }
        }
        fflush(stderr);
    }).detach();
}

#elif defined(__linux__)

// Linux: the crash trace, and every thread's stack for --stacks and when the game is ended from outside. The
// executable links with -rdynamic, so backtrace_symbols names the recompiled functions; --profile, --watch and
// --hwwatch are Windows only and do nothing here.
//
// A thread's stack can only be walked from that thread, so dump_all_stacks sends each one SIGUSR2 in turn and its
// handler writes its own backtrace. A hung game is ended in one of two ways, and both print every thread's stack,
// which shows where it was stuck. Killed, it gets SIGTERM. Force-closed under X (the Steam Deck's game mode too) it
// gets no signal: the X server drops its connection and Xlib calls its I/O error handler, which must not return.
// Xlib's own calls exit() while the other threads are still going, and RmlUi's static destructors then crashed under
// them; GTK's, set when the file dialogs' GTK opens the display, calls _exit(). install_x_error_handler sets this
// file's after that, and it prints the stacks and ends the process.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <map>
#include <mutex>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {

std::mutex dump_mutex;

// Linux thread id -> game thread, recorded by game_thread_name (ultramodern calls it on each new game thread).
std::mutex game_threads_mutex;
std::map<pid_t, const OSThread*> game_threads;

std::atomic<bool> stack_written{ false };

pid_t current_tid() {
    return (pid_t)syscall(SYS_gettid);
}

void stack_signal(int) {
    void* frames[64];
    int depth = backtrace(frames, 64);
    // Without this handler's own frame.
    backtrace_symbols_fd(frames + 1, depth - 1, STDERR_FILENO);
    stack_written = true;
}

// with_self: this thread's too, where the process is ending (the main thread, in exit() or the SIGTERM handler).
void dump_all_stacks(bool with_self) {
    std::lock_guard lock{ dump_mutex };
    pid_t self = current_tid();
    fflush(stdout);
    fprintf(stderr, "[stacks]\n");
    if (DIR* dir = opendir("/proc/self/task")) {
        while (dirent* entry = readdir(dir)) {
            pid_t tid = (pid_t)atoi(entry->d_name);
            if (tid <= 0 || (tid == self && !with_self)) {
                continue;
            }
            char name[32] = "";
            char path[64];
            snprintf(path, sizeof(path), "/proc/self/task/%d/comm", (int)tid);
            if (FILE* comm = fopen(path, "r")) {
                if (fgets(name, sizeof(name), comm) != nullptr) {
                    name[strcspn(name, "\n")] = '\0';
                }
                fclose(comm);
            }
            // Threads that block every signal (Mesa's) never answer.
            bool blocks = false;
            snprintf(path, sizeof(path), "/proc/self/task/%d/status", (int)tid);
            if (FILE* status = fopen(path, "r")) {
                char line[128];
                unsigned long long mask;
                while (fgets(line, sizeof(line), status) != nullptr) {
                    if (sscanf(line, "SigBlk: %llx", &mask) == 1) {
                        blocks = (mask >> (SIGUSR2 - 1)) & 1;
                        break;
                    }
                }
                fclose(status);
            }
            fprintf(stderr, "  %5d %s:", (int)tid, name);
            {
                std::lock_guard threads_lock{ game_threads_mutex };
                auto it = game_threads.find(tid);
                if (it != game_threads.end()) {
                    const OSThread* gt = it->second;
                    fprintf(stderr, " [id %d pri %d %s%s]", gt->id, gt->priority, state_name(gt->state),
                            gt->queue == (PTR(PTR(OSThread)))-1 ? " in run queue" : "");
                }
            }
            fprintf(stderr, blocks && tid != self ? " (blocks signals)\n" : "\n");
            fflush(stderr);
            stack_written = false;
            if (blocks && tid != self) {
                continue;
            }
            if (tid == self) {
                void* frames[64];
                backtrace_symbols_fd(frames, backtrace(frames, 64), STDERR_FILENO);
            }
            else if (syscall(SYS_tgkill, getpid(), tid, SIGUSR2) == 0) {
                for (int i = 0; i < 200 && !stack_written; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        closedir(dir);
    }
    rush2049::print_probes();
    fflush(stderr);
}

// Its argument is the Display*.
int x_io_error(void*) {
    fprintf(stderr, "\n[exit] the X server closed the game's connection; every thread:\n");
    dump_all_stacks(true);
    _exit(EXIT_FAILURE);
}

// Not async-signal-safe, but the process is ending either way.
void term_signal(int sig) {
    fprintf(stderr, "\n[exit] signal %d (%s); every thread:\n", sig, strsignal(sig));
    dump_all_stacks(true);
    _exit(128 + sig);
}

void crash_signal(int sig, siginfo_t* info, void*) {
    fprintf(stderr, "\n[crash] signal %d (%s) at address %p\n", sig, strsignal(sig), info->si_addr);
    void* frames[64];
    int depth = backtrace(frames, 64);
    backtrace_symbols_fd(frames, depth, STDERR_FILENO);
    rush2049::print_probes();
    fflush(stderr);
    // SA_RESETHAND put the default action back, so re-raising ends the process the usual way (core dump included).
    raise(sig);
}

} // namespace

void rush2049::install_crash_handler() {
    // Load libgcc's unwinder now: backtrace() would otherwise dlopen it inside the handler.
    void* warmup[1];
    backtrace(warmup, 1);
    struct sigaction sa{};
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT }) {
        sigaction(sig, &sa, nullptr);
    }
    struct sigaction stack{};
    stack.sa_handler = stack_signal;
    stack.sa_flags = SA_RESTART;
    sigemptyset(&stack.sa_mask);
    sigaction(SIGUSR2, &stack, nullptr);
    signal(SIGTERM, term_signal);
}

void rush2049::install_x_error_handler() {
    // SDL loads libX11 itself when it opens a window, so it is looked up rather than linked.
    void* x11 = dlopen("libX11.so.6", RTLD_NOW);
    using Handler = int (*)(void*);
    auto set_handler = x11 ? (Handler(*)(Handler))dlsym(x11, "XSetIOErrorHandler") : nullptr;
    if (set_handler != nullptr) {
        set_handler(x_io_error);
    }
}

std::string rush2049::game_thread_name(const OSThread* t) {
    {
        std::lock_guard lock{ game_threads_mutex };
        game_threads[current_tid()] = t;
    }
    return "Game Thread " + std::to_string(t->id);
}

void rush2049::start_stack_sampler(int interval_seconds, bool stacks) {
    std::thread([interval_seconds, stacks]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
            if (stacks) {
                dump_all_stacks(false);
            }
            else {
                rush2049::print_probes();
                fflush(stderr);
            }
        }
    }).detach();
}

void rush2049::start_hw_watch(uint32_t) {}
void rush2049::start_profiler(int, int) {}
void rush2049::start_byte_watch(uint32_t) {}

#endif

#if defined(_WIN32) || defined(__linux__)

#include <cstdio>
#include <ctime>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

bool stderr_goes_nowhere() {
#ifdef _WIN32
    // A window program started without a redirect has no standard handles.
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    return h == nullptr || h == INVALID_HANDLE_VALUE;
#else
    // Only a terminal is somebody watching. A desktop launcher hands it /dev/null or the journal's socket, and Steam
    // a pipe into its own console log, which a player never finds; a redirect of one's own keeps it with
    // --show-console (main.cpp).
    return !isatty(STDERR_FILENO);
#endif
}

FILE* reopen(const std::filesystem::path& path, FILE* stream) {
#ifdef _WIN32
    return _wfreopen(path.c_str(), L"a", stream);
#else
    return freopen(path.c_str(), "a", stream);
#endif
}

} // namespace

void rush2049::start_log_file(const std::filesystem::path& dir, const char* first_line) {
    if (!stderr_goes_nowhere()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path log = dir / "rush2049.log";
    std::filesystem::rename(log, dir / "rush2049.old.log", ec);
    // Both streams append to the one file, unbuffered, so their lines land in order and a crash loses none.
    if (reopen(log, stdout) == nullptr || reopen(log, stderr) == nullptr) {
        return;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    time_t now = time(nullptr);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stderr, "%s, started %s\n", first_line, date);
}

#else

void rush2049::start_log_file(const std::filesystem::path&, const char*) {}

#endif

#if !defined(_WIN32) && !defined(__linux__)

void rush2049::start_hw_watch(uint32_t) {}
void rush2049::install_crash_handler() {}
void rush2049::install_x_error_handler() {}
std::string rush2049::game_thread_name(const OSThread* t) { return "Game Thread " + std::to_string(t->id); }
void rush2049::start_stack_sampler(int, bool) {}
void rush2049::start_profiler(int, int) {}
void rush2049::start_byte_watch(uint32_t) {}

#endif
