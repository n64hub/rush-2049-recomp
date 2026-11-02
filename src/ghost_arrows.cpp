// Ghost Arrows (Graphics tab). The arrow floating over each ghost in a Ghost Race is a model like the scenery's, drawn
// by the model draw (func_8009C8F0) after the rest of the scene and tested against the depth buffer pixel by pixel, so
// anything nearer the camera covers the part of the arrow behind it: on Track 1 the deck over the road beside the
// footbridge straight cuts the top off the arrow of a ghost driving under it, in plain view.
//
// At Whole the arrow is drawn whole or not at all, by whether its tip can be seen. The model is a disc 145 units
// across its x-y plane (y up, turning about it) with the arrow, pointing down, in its texture, so the tip is the
// disc's lowest vertex, (0, -144), the one nearest the ghost. A ghost going over a crest keeps its whole arrow until
// the tip drops behind the road, then loses all of it at once, where the original cut it off from the bottom up. The
// hook before the model's display list call writes, at the display list pointer (sp+0xE4, which the function reads
// back for every command after it):
//   G_VTX of that one vertex, from the arrow's own vertex data, into slot 0
//   gEXVertexZTest(0)    RT64 tests the vertex against the depth buffer as the scene has left it, and skips every
//                        triangle up to the end of the test if something nearer covers it
//   zsrcsel prim, prim depth 0 (the arrow's list doesn't set either), so the arrow is in front of everything
// and the hook after it gEXEndVertexZTest, zsrcsel pixel, and the prim depth the game last set (func_8008A644 keeps it
// in 0x8012E678's low half, and only sets it again when it changes). The test is RT64's (rt64_gbi_extended.cpp,
// RSPVertexTestZCS.hlsl), on the GPU, so nothing waits for the frame; its draws are left in place by See-Through Order.
// RT64 reads extended commands only after a gEXEnable in the same display list, which src/widescreen.cpp writes at
// the start of every frame's list while this is on.
//
// The arrow is told from other models by its display list: a 32-vertex load, a texture list, the render mode
// C8104A50 (see-through, depth tested, not written) and the combiner FC127E03 FF0FF3FF, with a prim colour (node flag
// 0x2000), the ghost's colour. Every ghost's arrow is the same model.

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "rush2049_game.h"
#include "rush2049_hooks.h"
#include "../lib/rt64/include/rt64_extended_gbi.h"

namespace {

// Set from the settings menu, read on the game thread. It starts at the option's default (config.cpp).
std::atomic<bool> whole{ true };

// The arrow being drawn, between the two hooks.
} // namespace
int r49_arrow_log = 0; // R49 TEMP
namespace {
bool in_arrow = false;

uint32_t read_word(uint8_t* rdram, uint32_t addr) {
    return (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
}

void write_word(uint8_t* rdram, uint32_t addr, uint32_t value) {
    MEM_W(0, (gpr)(int32_t)addr) = (int32_t)value;
}

int16_t read_half(uint8_t* rdram, uint32_t addr) {
    return (int16_t)MEM_H(0, (gpr)(int32_t)addr);
}

bool is_arrow(uint8_t* rdram, uint32_t node, uint32_t dl) {
    return (read_word(rdram, node) & 0x2000) != 0 &&
        read_word(rdram, dl) == 0x01020040 && read_word(rdram, dl + 8) == 0xDE000000 &&
        read_word(rdram, dl + 16) == 0xE200001C && read_word(rdram, dl + 20) == 0xC8104A50 &&
        read_word(rdram, dl + 24) == 0xFC127E03 && read_word(rdram, dl + 28) == 0xFF0FF3FF;
}

uint32_t put(uint8_t* rdram, uint32_t gp, uint32_t w0, uint32_t w1) {
    write_word(rdram, gp, w0);
    write_word(rdram, gp + 4, w1);
    return gp + 8;
}

uint32_t put(uint8_t* rdram, uint32_t gp, const GfxCommand& cmd) {
    return put(rdram, gp, cmd.values.word0, cmd.values.word1);
}

} // namespace

void rush2049::set_ghost_arrows_whole(bool on) {
    whole.store(on, std::memory_order_relaxed);
}

bool rush2049::ghost_arrows_whole() {
    return whole.load(std::memory_order_relaxed);
}

extern "C" void rush2049_ghost_arrow_begin(uint8_t* rdram, recomp_context* ctx) {
    in_arrow = false;
    const uint32_t node = (uint32_t)ctx->r23;
    const uint32_t dl = (uint32_t)ctx->r30;
    if (!rush2049::ghost_arrows_whole() || !is_arrow(rdram, node, dl)) {
        return;
    }
    if (r49_arrow_log > 0) { // R49 TEMP
        r49_arrow_log--;
        fprintf(stderr, "[arrow] node %08X dl %08X tex %08X vtx %08X |", node, dl, read_word(rdram, dl + 12), read_word(rdram, dl + 4));
        for (uint32_t o = 0; o < 0x50; o += 4) fprintf(stderr, " %08X", read_word(rdram, node + o));
        fprintf(stderr, "%c", 10);
    }

    // The tip: the lowest of the first load's vertices (the model's y is up).
    const uint32_t vertices = read_word(rdram, dl + 4) | 0x80000000;
    uint32_t tip = vertices;
    for (uint32_t v = vertices; v < vertices + 32 * 16; v += 16) {
        if (read_half(rdram, v + 2) < read_half(rdram, tip + 2)) {
            tip = v;
        }
    }

    const uint32_t sp = (uint32_t)ctx->r29;
    uint32_t gp = read_word(rdram, sp + 0xE4);
    gp = put(rdram, gp, 0x01001002, tip - 0x80000000);  // gsSPVertex(tip, 1, 0), segment 0 like the game's own
    GfxCommand test{};
    gEXVertexZTest(&test, 0);
    gp = put(rdram, gp, test);
    gp = put(rdram, gp, 0xE2001D00, 0x00000004);  // gsDPSetDepthSource(G_ZS_PRIM)
    gp = put(rdram, gp, 0xEE000000, 0x00000000);  // gsDPSetPrimDepth(0, 0)
    write_word(rdram, sp + 0xE4, gp);
    in_arrow = true;
}

extern "C" void rush2049_ghost_arrow_end(uint8_t* rdram, recomp_context* ctx) {
    if (!in_arrow) {
        return;
    }
    in_arrow = false;
    const uint32_t sp = (uint32_t)ctx->r29;
    uint32_t gp = read_word(rdram, sp + 0xE4);
    GfxCommand end{};
    gEXEndVertexZTest(&end);
    gp = put(rdram, gp, end);
    gp = put(rdram, gp, 0xE2001D00, 0x00000000);  // gsDPSetDepthSource(G_ZS_PIXEL)
    gp = put(rdram, gp, 0xEE000000, (read_word(rdram, 0x8012E678) & 0xFFFF) << 16);
    write_word(rdram, sp + 0xE4, gp);
}
