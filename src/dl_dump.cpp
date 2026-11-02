// --dump-dl <file> <seconds>: write one F3DEX2 display list to a text file, the first graphics task the game starts
// after <seconds>. For finding how the game draws things (HUD, menus) when deciding where to hook.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "recomp.h"
#include "rush2049_hooks.h"
#include "rush2049_game.h"

namespace {

std::string dump_path;
double dump_after_s = 0;
std::atomic<bool> dump_armed{ false };
std::chrono::steady_clock::time_point start_time;

struct Walker {
    uint8_t* rdram;
    FILE* f;
    uint32_t segments[16]{};
    int lines = 0;

    uint32_t phys(uint32_t addr) const {
        if (addr >= 0x81000000 && addr < 0xA0000000) {  // the recomp heap, which RT64 reads in extended RDRAM mode
            return addr - 0x80000000;
        }
        uint32_t p = segments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF);
        return p & 0x007FFFFF;
    }
    uint32_t rd32(uint32_t p) const { return *reinterpret_cast<uint32_t*>(rdram + (p & ~3u)); }
    int16_t rd16(uint32_t p) const { return *reinterpret_cast<int16_t*>(rdram + (p ^ 2)); }
    uint16_t rdu16(uint32_t p) const { return *reinterpret_cast<uint16_t*>(rdram + (p ^ 2)); }

    void matrix(uint32_t p, int depth) {
        for (int r = 0; r < 4; r++) {
            fprintf(f, "%*s    [", depth * 2, "");
            for (int c = 0; c < 4; c++) {
                int i = r * 4 + c;
                double v = rd16(p + i * 2) + rdu16(p + 32 + i * 2) / 65536.0;
                fprintf(f, " %10.4f", v);
            }
            fprintf(f, " ]\n");
        }
    }

    void walk(uint32_t addr, int depth) {
        if (depth > 12) {
            return;
        }
        uint32_t p = phys(addr);
        for (int n = 0; n < 20000 && lines < 200000; n++, p += 8, lines++) {
            uint32_t w0 = rd32(p), w1 = rd32(p + 4);
            uint8_t op = w0 >> 24;
            fprintf(f, "%*s%08X: %08X %08X", depth * 2, "", p | 0x80000000, w0, w1);
            switch (op) {
            case 0x01: {  // G_VTX
                uint32_t count = (w0 >> 12) & 0xFF;
                uint32_t vp = phys(w1);
                fprintf(f, "  VTX n=%u ->", count);
                for (uint32_t i = 0; i < count && i < 4; i++) {
                    fprintf(f, " (%d,%d,%d)", rd16(vp + i * 16), rd16(vp + i * 16 + 2), rd16(vp + i * 16 + 4));
                }
                fprintf(f, "\n");
                break;
            }
            case 0xDA:  // G_MTX
                fprintf(f, "  MTX %s %s %s\n", (w0 & 4) ? "PROJ" : "MODELVIEW", (w0 & 2) ? "LOAD" : "MUL",
                        (w0 & 1) ? "NOPUSH" : "PUSH");
                matrix(phys(w1), depth);
                break;
            case 0xDB:  // G_MOVEWORD
                if (((w0 >> 16) & 0xFF) == 0x06) {
                    segments[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFF;
                    fprintf(f, "  SEGMENT %u = %08X\n", (w0 & 0xFFFF) / 4, w1);
                } else {
                    fprintf(f, "  MOVEWORD\n");
                }
                break;
            case 0xDC: {  // G_MOVEMEM
                uint32_t vp = phys(w1);
                if ((w0 & 0xFF) == 0x08) {
                    fprintf(f, "  VIEWPORT scale (%d,%d) trans (%d,%d)\n", rd16(vp) / 4, rd16(vp + 2) / 4,
                            rd16(vp + 8) / 4, rd16(vp + 10) / 4);
                } else {
                    fprintf(f, "  MOVEMEM idx %u\n", w0 & 0xFF);
                }
                break;
            }
            case 0xDE:  // G_DL
                fprintf(f, "  DL %s\n", (w0 >> 16) & 1 ? "BRANCH" : "CALL");
                if ((w0 >> 16) & 1) {
                    p = phys(w1) - 8;  // a jump: continue there at the same depth
                } else {
                    walk(w1, depth + 1);
                }
                break;
            case 0xDF:
                fprintf(f, "  ENDDL\n");
                return;
            case 0xE4:
                fprintf(f, "  TEXRECT (%.2f,%.2f)-(%.2f,%.2f) tile %u\n", ((w1 >> 12) & 0xFFF) / 4.0,
                        (w1 & 0xFFF) / 4.0, ((w0 >> 12) & 0xFFF) / 4.0, (w0 & 0xFFF) / 4.0, (w1 >> 24) & 7);
                break;
            case 0xF6:
                fprintf(f, "  FILLRECT (%u,%u)-(%u,%u)\n", (w1 >> 14) & 0x3FF, (w1 >> 2) & 0x3FF, (w0 >> 14) & 0x3FF,
                        (w0 >> 2) & 0x3FF);
                break;
            case 0xED:
                fprintf(f, "  SCISSOR (%u,%u)-(%u,%u)\n", (w0 >> 14) & 0x3FF, (w0 >> 2) & 0x3FF, (w1 >> 14) & 0x3FF,
                        (w1 >> 2) & 0x3FF);
                break;
            case 0xFD:
                fprintf(f, "  SETTIMG %08X\n", w1);
                break;
            case 0x05: fprintf(f, "  TRI1\n"); break;
            case 0x06: fprintf(f, "  TRI2\n"); break;
            case 0x07: fprintf(f, "  QUAD\n"); break;
            case 0xD8: fprintf(f, "  POPMTX\n"); break;
            default: fprintf(f, "\n"); break;
            }
        }
    }
};

} // namespace

void rush2049::start_dl_dump(const char* path, double after_seconds) {
    dump_path = path;
    dump_after_s = after_seconds;
    start_time = std::chrono::steady_clock::now();
    dump_armed = true;
}

std::string ram_dump_prefix;
std::vector<double> ram_dump_times;
size_t ram_dumps_done = 0;

void rush2049::start_ram_dump(const char* prefix, const char* times) {
    ram_dump_prefix = prefix;
    for (const char* p = times; *p != '\0';) {
        char* end;
        ram_dump_times.push_back(strtod(p, &end));
        p = (*end == ',') ? end + 1 : end;
        if (end == p && *p != '\0') {
            break;
        }
    }
    start_time = std::chrono::steady_clock::now();
}

static void dump_ram(uint8_t* rdram) {
    if (ram_dumps_done >= ram_dump_times.size()) {
        return;
    }
    std::chrono::duration<double> since = std::chrono::steady_clock::now() - start_time;
    if (since.count() < ram_dump_times[ram_dumps_done]) {
        return;
    }
    std::string path = ram_dump_prefix + std::to_string(ram_dumps_done) + ".bin";
    ram_dumps_done++;
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    // Words are host-endian in rdram; write them big-endian, as the N64 sees memory.
    std::vector<uint8_t> out(8 * 1024 * 1024);
    for (size_t i = 0; i < out.size(); i += 4) {
        uint32_t w = *reinterpret_cast<uint32_t*>(rdram + i);
        out[i + 0] = w >> 24;
        out[i + 1] = w >> 16;
        out[i + 2] = w >> 8;
        out[i + 3] = w;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    fprintf(stderr, "[ram] wrote %s at %.2f s\n", path.c_str(), since.count());
}

extern "C" void rush2049_dump_task(uint8_t* rdram, uint32_t task_addr) {
    dump_ram(rdram);
    if (!dump_armed.load(std::memory_order_relaxed)) {
        return;
    }
    std::chrono::duration<double> since = std::chrono::steady_clock::now() - start_time;
    gpr task = (gpr)(int32_t)task_addr;
    if (since.count() < dump_after_s || MEM_W(0x00, task) != 1) {  // M_GFXTASK
        return;
    }
    dump_armed = false;
    FILE* f = fopen(dump_path.c_str(), "w");
    if (f == nullptr) {
        return;
    }
    uint32_t data_ptr = (uint32_t)MEM_W(0x30, task);
    fprintf(f, "gfx task %08X data_ptr %08X at %.2f s\n", task_addr, data_ptr, since.count());
    Walker w{ rdram, f };
    w.walk(data_ptr | 0x80000000, 0);  // in the recomp heap (src/frame_list.cpp)
    fclose(f);
    fprintf(stderr, "[dl] wrote %s (%d lines)\n", dump_path.c_str(), w.lines);
}
