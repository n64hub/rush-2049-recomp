// The camera's place, handed to RT64 so that the frames it draws between the game's own know where the camera is, and
// a camera that cut to another shot pairs nothing across the cut (RT64_SKIP_CAMERA_CUTS, CMakeLists.txt).
//
// The scene draw func_8009F058 loads, for each camera, a bare perspective projection (G_MTX projection, load) and then
// the camera's turn (G_MTX projection, multiply: a rotation only), and the camera's place goes into every model's
// matrix instead: the node matrix builders write each node's offset from the camera, times 16 (s15.16, the 2048-unit
// limit in src/draw_distance.cpp). So RT64 found a view that turned but never moved, its eye always at the origin, and
// its cut test (rt64_game_frame.cpp, isCameraCut) could only see a cut that turned the camera more than 45 degrees: the
// attract race's chase cameras, which cut from one car to another facing about the same way, were drawn in the frames
// between as one shot smeared into the other.
//
// RT64's extended view matrix (gEXSetViewMatrixFloat) is for such a game: RT64 takes each world matrix as the modelview
// times that matrix, and the view as its inverse times the view it found, which draws the same picture. Given the
// translation by the camera's place times 16, a node's world matrix is where it stands in the world (the scenery stands
// still), and the view is the camera's whole view, its eye where the camera is. Right after the camera's turn is written
// to the display list (at the list pointer, $s7), the matrix goes in a slot of a ring in the recomp heap, which RT64
// reads in the extended RDRAM mode the frame's list sets (src/frame_list.cpp), and gEXSetViewMatrixFloat after the turn.
// The ring holds the views of many frames, so a slot is never written while RT64 can still be reading its frame. At the
// end of the camera's scene, after the ghost arrows (src/ghost_arrows.cpp), the view goes back to the identity, so the
// 2D drawn after the scene is not taken for something in the world.
//
// The camera's place is what func_8009F058 hands func_8009EBC0 (its fourth argument + 0x24, three floats in world
// units), the place it builds its nodes' offsets from.

#include <cstdint>
#include <cstring>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "rush2049_hooks.h"

#define F3DEX_GBI_2
#include "../lib/rt64/include/rt64_extended_gbi.h"

namespace {

constexpr float list_units_per_world_unit = 16.0f;
constexpr uint32_t slot_bytes = 64;  // a float matrix
constexpr uint32_t slot_count = 64;  // four cameras a frame, so 16 frames before a slot is written again
constexpr uint32_t identity_slot = 0;

uint32_t ring = 0;  // the ring's RDRAM address, 0 until made (or if there was no room)
uint32_t next_slot = 1;
gpr camera_place = 0;

void write_word(uint8_t* rdram, uint32_t addr, uint32_t value) {
    MEM_W(0, (gpr)(int32_t)addr) = (int32_t)value;
}

void write_matrix(uint8_t* rdram, uint32_t addr, const float (&m)[16]) {
    for (int i = 0; i < 16; i++) {
        uint32_t bits;
        std::memcpy(&bits, &m[i], sizeof(bits));
        write_word(rdram, addr + i * 4, bits);
    }
}

bool make_ring(uint8_t* rdram) {
    if (ring == 0) {
        void* p = recomp::alloc(rdram, slot_bytes * slot_count);
        if (p == nullptr) {
            return false;
        }
        ring = (uint32_t)((uint8_t*)p - rdram) + 0x80000000;
        const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        write_matrix(rdram, ring + identity_slot * slot_bytes, identity);
    }
    return true;
}

void append_view(uint8_t* rdram, recomp_context* ctx, uint32_t matrix) {
    GfxCommand cmd[1]{};
    gEXSetViewMatrixFloat(&cmd[0], matrix);
    uint32_t list_ptr = (uint32_t)ctx->r23;
    uint32_t list = (uint32_t)MEM_W(0, (gpr)(int32_t)list_ptr);
    write_word(rdram, list, cmd[0].values.word0);
    write_word(rdram, list + 4, cmd[0].values.word1);
    write_word(rdram, list_ptr, list + 8);
}

}  // namespace

extern "C" void rush2049_camera_view_place(gpr place) {
    camera_place = place;
}

extern "C" void rush2049_camera_view_begin(uint8_t* rdram, recomp_context* ctx) {
    if (camera_place == 0 || !make_ring(rdram)) {
        return;
    }
    float place[3];
    for (int i = 0; i < 3; i++) {
        uint32_t bits = (uint32_t)MEM_W(i * 4, camera_place);
        std::memcpy(&place[i], &bits, sizeof(place[i]));
    }
    const float m[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        place[0] * list_units_per_world_unit, place[1] * list_units_per_world_unit, place[2] * list_units_per_world_unit, 1,
    };
    uint32_t slot = ring + next_slot * slot_bytes;
    next_slot = (next_slot + 1 < slot_count) ? next_slot + 1 : 1;
    write_matrix(rdram, slot, m);
    append_view(rdram, ctx, slot);
}

extern "C" void rush2049_camera_view_end(uint8_t* rdram, recomp_context* ctx) {
    if (ring == 0) {
        return;
    }
    append_view(rdram, ctx, ring + identity_slot * slot_bytes);
}
