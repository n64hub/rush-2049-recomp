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
//
// The frame's matrices. Each frame also takes a 64-byte matrix for every scenery node it draws from its block of frame
// data: the pointer at 0x8015B260 is set to the block + 0x6C0 where the list slot is set up, and func_8009DD88 adds 64
// to it for each node, with no end. A block has room for about 1100, which one or two views never reach, but four
// views of a city track pass it: the last matrices are written over the other block's start, its viewports among
// them, while the renderer reads that frame, and from the third view on a view's scenery goes missing for a frame and
// it flashes the colour of the sky. So each block's matrices go in a buffer of their own in the recomp heap too, with
// room for four times as many, which the extended RDRAM mode above lets RT64 read. The pointer is set there in the
// same hooks, right after the game sets it.
//
// The frame's 2D list. Text, the HUD and the result screens go in a second list, which the top level calls: two of
// 0x4B00 bytes (2400 commands) from 0x80124FE8, used in turn (func_800A4934 points 0x80149438 at one, func_800A48C8
// closes it and returns its start), and nothing checks their end either. The second is followed by the track's
// table of computer-car routes at 0x8012E5E8. Four players' result screens, where every view draws its own times
// over the race, pass 2400 commands: the first list runs into the second, the views show the wrong frame's colours,
// and the second runs into the route table, so the computer cars, still driving behind the screens, read a route
// from nowhere and the game crashes. So the 2D lists go in heap buffers with four times the room as well.

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
constexpr uint32_t matrix_pointer_addr = 0x8015B260;  // where the frame's next node matrix goes
constexpr uint32_t matrix_bytes = 4096 * 64;

// A buffer for each slot, by the slot's address (the game has two).
struct Buffer {
    uint32_t slot = 0;
    uint32_t list = 0;
    uint32_t matrices = 0;
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
            void* matrices = recomp::alloc(rdram, matrix_bytes);
            b.slot = slot;
            b.list = (uint32_t)((uint8_t*)list - rdram) + 0x80000000;
            b.matrices = matrices ? (uint32_t)((uint8_t*)matrices - rdram) + 0x80000000 : 0;
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

constexpr uint32_t hud_list_ptr_addr = 0x80149438;
constexpr uint32_t hud_lists_addr = 0x80124FE8;
constexpr uint32_t hud_list_bytes = 0x4B00;
constexpr int hud_list_count = 2;
uint32_t hud_lists[hud_list_count];

uint32_t hud_list_for(uint8_t* rdram, uint32_t index) {
    if (index >= hud_list_count) {
        return 0;
    }
    if (hud_lists[index] == 0) {
        void* list = recomp::alloc(rdram, hud_list_bytes * 4);
        if (list == nullptr) {
            return 0;
        }
        hud_lists[index] = (uint32_t)((uint8_t*)list - rdram) + 0x80000000;
    }
    return hud_lists[index];
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

// The matrix buffer of the block whose list slot is at slot; 0 if it has none.
uint32_t matrices_for(uint32_t slot) {
    for (const Buffer& b : buffers) {
        if (b.slot == slot) {
            return b.matrices;
        }
    }
    return 0;
}

extern "C" gpr rush2049_frame_list(uint8_t* rdram, gpr slot) {
    uint32_t list = buffer_for(rdram, (uint32_t)slot);
    if (uint32_t matrices = matrices_for((uint32_t)slot); matrices != 0) {
        write_word(rdram, matrix_pointer_addr, matrices);
    }
    return list != 0 ? (gpr)(int32_t)(list + prefix_bytes) : slot;
}

extern "C" gpr rush2049_frame_task_list(uint8_t* rdram, gpr slot) {
    uint32_t list = buffer_for(rdram, (uint32_t)slot);
    return list != 0 ? (gpr)(int32_t)list : slot;
}

extern "C" gpr rush2049_frame_list_bytes(uint8_t* rdram, gpr bytes) {
    uint32_t slot = (uint32_t)MEM_W(0, (gpr)(int32_t)frame_data_addr) + slot_offset;
    uint32_t list = buffer_for(rdram, slot);
    if (list == 0) {
        return bytes;
    }
    uint32_t head = (uint32_t)MEM_W(0, (gpr)(int32_t)dl_head_addr);
    return (gpr)(int32_t)((head - list) & ~7u);
}

extern "C" void rush2049_hud_list_opened(uint8_t* rdram, gpr index) {
    if (uint32_t list = hud_list_for(rdram, (uint32_t)index); list != 0) {
        write_word(rdram, hud_list_ptr_addr, list);
    }
}

extern "C" gpr rush2049_hud_list_closed(uint8_t* rdram, gpr start) {
    uint32_t index = ((uint32_t)start - hud_lists_addr) / hud_list_bytes;
    uint32_t list = hud_list_for(rdram, index);
    return list != 0 ? (gpr)(int32_t)list : start;
}
