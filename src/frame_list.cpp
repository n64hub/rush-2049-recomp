// The frame's display list. The game keeps two blocks of frame data (viewports, matrices, the display list), at
// 0x800586D0 and 0x8006A090, and builds each frame in the other one, so the renderer can read one frame while the
// next is written; 0x8015B250 holds the one being built. Each block's top-level display list is a fixed slot 0x9CC0
// into it with room for 4000 commands (0x7D00 bytes), and nothing checks the end: the first slot is followed by the
// second block's data. The top level holds a command for each model and view, and a two-player race at the default
// settings already fills over 3600 of them, so with Scenery Culling Off, or anything else that draws more, a frame can
// pass 4000 and run into the other frame's data, which is written over the list's tail in turn. RT64 then reads that
// data as commands: a microcode load from nowhere (no GBI matches, and the next command crashes), a tile load whose
// rows run backwards through all of RDRAM, or a list that never ends. The picture freezes for good while the sound
// plays on, or the game crashes.
//
// So each slot's list goes in a buffer of its own in the recomp heap, above the game's 8 MB, with room for four times
// as many commands. The hooks (us.toml) are where the game points its display list pointer (0x801497C8) at a slot, at
// the start of each frame (func_800EDDC0) and where the frame data is set up (func_800EE8B4, func_800EEA7C), and where
// it puts the slot's address and the list's length in the frame's graphics task (func_800EDACC).
//
// RecompFrontend hands RT64 the task's list as an offset into RDRAM, so RT64 reads the top level from the heap as it
// is. But the game also writes small lists inside the top-level list, between its commands, and calls them by their
// address, which RT64 masks to the first 8 MB unless it is in extended RDRAM mode. So the buffer starts with
// gEXEnable and gEXSetRDRAMExtended(1), and the game's list follows them: in that mode RT64 reads an address with
// the top bit set (0x80000000 and up) as physical, which for the game's own addresses, all in 0x80000000-0x80800000,
// is what masking them gave. The task's list is the buffer's start; the game is pointed after the two commands.

#include <cstdint>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "rush2049_game.h"
#include "rush2049_hooks.h"

#define F3DEX_GBI_2
#include "../lib/rt64/include/rt64_extended_gbi.h"

namespace {

constexpr uint32_t dl_head_addr = 0x801497C8;     // the display list pointer
constexpr uint32_t frame_data_addr = 0x8015B250;  // the frame data being built
constexpr uint32_t slot_offset = 0x9CC0;
constexpr uint32_t prefix_bytes = 2 * 8;
constexpr uint32_t list_bytes = prefix_bytes + 16000 * 8;

// A buffer for each slot, by the slot's address (the game has two).
struct Buffer {
    uint32_t slot = 0;
    uint32_t list = 0;
};
Buffer buffers[4];

void write_word(uint8_t* rdram, uint32_t addr, uint32_t value) {
    MEM_W(0, (gpr)(int32_t)addr) = (int32_t)value;
}

// The buffer for the list slot at slot, made the first time; 0 if there is no room for it.
uint32_t buffer_for(uint8_t* rdram, uint32_t slot) {
    for (Buffer& b : buffers) {
        if (b.slot == slot) {
            return b.list;
        }
        if (b.slot == 0) {
            void* list = recomp::alloc(rdram, list_bytes);
            if (list == nullptr) {
                return 0;
            }
            b.slot = slot;
            b.list = (uint32_t)((uint8_t*)list - rdram) + 0x80000000;
            GfxCommand prefix[2]{};
            gEXEnable(&prefix[0]);
            gEXSetRDRAMExtended(&prefix[1], 1);
            for (int i = 0; i < 2; i++) {
                write_word(rdram, b.list + i * 8, prefix[i].values.word0);
                write_word(rdram, b.list + i * 8 + 4, prefix[i].values.word1);
            }
            return b.list;
        }
    }
    return 0;
}

}  // namespace

uint32_t rush2049::frame_list_game_start(uint32_t task_list) {
    for (const Buffer& b : buffers) {
        if (b.list != 0 && b.list == task_list) {
            return task_list + prefix_bytes;
        }
    }
    return task_list;
}

extern "C" gpr rush2049_frame_list(uint8_t* rdram, gpr slot) {
    uint32_t list = buffer_for(rdram, (uint32_t)slot);
    return list != 0 ? (gpr)(int32_t)(list + prefix_bytes) : slot;
}

extern "C" gpr rush2049_frame_task_list(uint8_t* rdram, gpr slot) {
    uint32_t list = buffer_for(rdram, (uint32_t)slot);
    return list != 0 ? (gpr)(int32_t)list : slot;
}

extern "C" void dbg_frame_list_scan(uint8_t* rdram);
extern "C" gpr rush2049_frame_list_bytes(uint8_t* rdram, gpr bytes) {
    dbg_frame_list_scan(rdram);
    uint32_t slot = (uint32_t)MEM_W(0, (gpr)(int32_t)frame_data_addr) + slot_offset;
    uint32_t list = buffer_for(rdram, slot);
    if (list == 0) {
        return bytes;
    }
    uint32_t head = (uint32_t)MEM_W(0, (gpr)(int32_t)dl_head_addr);
    return (gpr)(int32_t)((head - list) & ~7u);
}

#include <cstdio>
static int dbg_prints = 0;
static void dbg_walk(uint8_t* rdram, uint32_t p, int depth) {  // TEMP DEBUG
    for (int n = 0; n < 40000 && dbg_prints < 100; n++, p += 8) {
        uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)p), w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)p);
        uint8_t op = w0 >> 24;
                if ((w1 >> 29) == 5 && op != 0xE4 && op != 0xE1 && op != 0xF1 && op != 0xF0 && op != 0xF2 && op != 0xF5 && op != 0xFC && op != 0xE2 && op != 0xE3 && op != 0xF7 && op != 0xFA && op != 0xF9 && op != 0xF8 && op != 0xFB && op != 0xED && op != 0xF6 && op != 0x05 && op != 0x06 && op != 0x07) {
            fprintf(stderr, "[fl] kseg1 d%d %08X: %08X %08X\n", depth, p, w0, w1); dbg_prints++;
        }
        if (op == 0xDF) return;
        if (op == 0xDE && depth < 6 && (w1 >> 24) >= 0x80 && (w1 >> 24) < 0x82) {
            if (w0 & 0x00010000) { p = w1 - 8; continue; }
            dbg_walk(rdram, w1, depth + 1);
        }
    }
}
extern "C" void dbg_frame_list_scan(uint8_t* rdram) {
    if (dbg_prints >= 100) return;
    uint32_t l = buffer_for(rdram, (uint32_t)MEM_W(0, (gpr)(int32_t)frame_data_addr) + slot_offset); if (l) dbg_walk(rdram, l + prefix_bytes, 0);
}
