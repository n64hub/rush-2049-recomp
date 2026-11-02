// The frame's display list. The game builds each frame's top-level display list in a fixed slot of its frame data
// (the frame data's address is in 0x8015B250, and the slot starts 0x9CC0 into it), with room for 4000 commands
// (0x7D00 bytes) and nothing checking the end: the frame data after it is overwritten by a longer list and writes
// over the list's tail in turn. The top level holds a command for each model and view, and a two-player race at the
// default settings already fills over 3600 of them, so with Scenery Culling Off, or anything else that draws more,
// a frame can pass 4000. RT64 then reads the frame data that landed there as commands: a microcode load from
// nowhere (no GBI matches, and the next command crashes), a tile load whose rows run backwards through all of RDRAM,
// or a list that never ends. The picture freezes for good while the sound plays on, or the game crashes.
//
// So the list goes in a buffer of its own in the recomp heap, above the game's 8 MB, with room for four times as many
// commands. The hooks (us.toml) are where the game points its display list pointer (0x801497C8) at the slot, at the
// start of each frame (func_800EDDC0) and where the frame data is set up (func_800EE8B4, func_800EEA7C), and where it
// puts the slot's address and the list's length in the frame's graphics task (func_800EDACC). RecompFrontend hands RT64
// the task's list as an offset into RDRAM, so RT64 reads a list in the heap as it is, extended RDRAM mode or not, and
// the lists it calls are where the game put them. The game builds every frame's list in the one slot, so one buffer
// does too.

#include <cstdint>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "rush2049_hooks.h"

namespace {

constexpr uint32_t dl_head_addr = 0x801497C8;  // the display list pointer
constexpr uint32_t list_bytes = 16000 * 8;
uint32_t frame_list = 0;

}  // namespace

extern "C" gpr rush2049_frame_list(uint8_t* rdram, gpr slot) {
    if (frame_list == 0) {
        void* list = recomp::alloc(rdram, list_bytes);
        if (list == nullptr) {
            return slot;
        }
        frame_list = (uint32_t)((uint8_t*)list - rdram) + 0x80000000;
    }
    return (gpr)(int32_t)frame_list;
}

extern "C" gpr rush2049_frame_list_bytes(uint8_t* rdram, gpr bytes) {
    if (frame_list == 0) {
        return bytes;
    }
    uint32_t head = (uint32_t)MEM_W(0, (gpr)(int32_t)dl_head_addr);
    return (gpr)(int32_t)((head - frame_list) & ~7u);
}
