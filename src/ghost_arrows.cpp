// Ghost Arrows (Graphics tab). The arrow floating over each ghost in a Ghost Race is one of the game's "polys", the
// quads it builds every frame for the cars' shadows, skid marks and smoke, drawn by the scene draw (func_8009F058)
// for each camera between its first model tree and its last two. The arrow is see-through and tested against the
// depth buffer without writing it, so everything in those last two trees is drawn over it, however far behind the
// ghost: on Track 1 the buildings beside the straight after the Slim Jim sign, and the signal arm on the bridge at its
// end, cut into the arrows of ghosts in plain view.
//
// At After Scenery the arrows are drawn after the last tree, still tested against the depth buffer pixel by pixel as
// the game draws them, so only what is between the camera and a part of an arrow covers that part: a sign the ghost
// passes under hides the piece of the arrow behind it, and a crest the ghost drops behind hides the arrow from below.
// The poly loop walks the list from 0x8015B268 (0x58 bytes each, the count at 0x801613B4) and writes each poly's
// commands at the display list pointer (s7 points at it): a pipe sync, its depth source, fog, prim colour, texture,
// render mode and combiner, the corners as vertices and the triangles. It only writes the state that has changed since
// the poly before it, by four caches: the depth source (sp+0x170), fog (sp+0x174, 1 on), the render mode kind (sp+0x178,
// -1 none) and the texture (0x80185508, which the texture load func_80099BFC is called for; it in turn sets the TLUT
// mode, cached at 0x80185638). The hook at the start of an arrow's turn (s6 the poly) sets those caches so that the
// arrow writes all of its state, and the hook at the end of the turn moves what it wrote to a buffer, takes the display
// list pointer back to where the arrow began and puts the caches back as they were, so the next poly writes what it
// needs as if the arrow had never been there. After the last tree (or where it would be) the hook writes the arrows,
// then fog on in two-cycle mode as the end of the poly loop leaves it, and clears the texture and TLUT caches as the
// game does after each tree. The arrows' draws are left in place by See-Through Order.
//
// The arrow is told from the other polys by its texture (0x9800: the first of the effects' set; the shadows are
// 0x9807, skid marks and smoke 0x9808), its four corners and its flags (0x1600 with the cameras it is seen by in the
// low bits: no fog, a quad, textured, see-through). Its colour is the ghost's.

#include <atomic>
#include <cstdint>

#include "rush2049_game.h"
#include "rush2049_hooks.h"

namespace {

// Set from the settings menu, read on the game thread. It starts at the option's default (config.cpp).
std::atomic<bool> after_scenery{ true };

// The arrow whose turn it is, between the two hooks: where its commands begin (0 when it isn't an arrow's turn) and
// the caches as they were before it.
uint32_t arrow_start = 0;
int32_t saved_depth_source;
int32_t saved_fog;
int32_t saved_render_mode;
uint32_t saved_texture;
int32_t saved_tlut;

// The arrows' commands, waiting for the end of the camera's scene. An arrow writes about 30.
constexpr uint32_t deferred_max = 1024;
uint32_t deferred[deferred_max * 2];
uint32_t deferred_count = 0;

constexpr uint32_t texture_cache = 0x80185508;
constexpr uint32_t tlut_cache = 0x80185638;

uint32_t read_word(uint8_t* rdram, uint32_t addr) {
    return (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
}

void write_word(uint8_t* rdram, uint32_t addr, uint32_t value) {
    MEM_W(0, (gpr)(int32_t)addr) = (int32_t)value;
}

bool is_arrow(uint8_t* rdram, uint32_t poly) {
    const uint32_t head = read_word(rdram, poly);
    return (head >> 16) == 4 && (head & 0xFFF0) == 0x1600 && (read_word(rdram, poly + 4) >> 16) == 0x9800;
}

void defer(uint32_t w0, uint32_t w1) {
    deferred[deferred_count * 2] = w0;
    deferred[deferred_count * 2 + 1] = w1;
    deferred_count++;
}

} // namespace

void rush2049::set_ghost_arrows_after_scenery(bool on) {
    after_scenery.store(on, std::memory_order_relaxed);
}

bool rush2049::ghost_arrows_after_scenery() {
    return after_scenery.load(std::memory_order_relaxed);
}

extern "C" void rush2049_ghost_poly_begin(uint8_t* rdram, recomp_context* ctx) {
    arrow_start = 0;
    const uint32_t sp = (uint32_t)ctx->r29;
    if (read_word(rdram, sp + 0x16C) == 0) {  // the camera's first poly
        deferred_count = 0;
    }
    const uint32_t poly = (uint32_t)ctx->r22;
    if (!rush2049::ghost_arrows_after_scenery() || !is_arrow(rdram, poly)) {
        return;
    }
    saved_depth_source = (int32_t)read_word(rdram, sp + 0x170);
    saved_fog = (int32_t)read_word(rdram, sp + 0x174);
    saved_render_mode = (int32_t)read_word(rdram, sp + 0x178);
    saved_texture = read_word(rdram, texture_cache);
    saved_tlut = (int32_t)read_word(rdram, tlut_cache);
    write_word(rdram, sp + 0x170, 0);                                            // writes zsrcsel pixel
    write_word(rdram, sp + 0x174, (read_word(rdram, poly) & 0x100) ? 1 : 0);     // writes fog off (or on)
    write_word(rdram, sp + 0x178, (uint32_t)-1);                                 // writes the render mode
    write_word(rdram, texture_cache, 0);                                         // loads the texture
    write_word(rdram, tlut_cache, (uint32_t)-1);                                 // writes the TLUT mode
    arrow_start = read_word(rdram, (uint32_t)ctx->r23);
}

extern "C" void rush2049_ghost_poly_end(uint8_t* rdram, recomp_context* ctx) {
    if (arrow_start == 0) {
        return;
    }
    const uint32_t start = arrow_start;
    arrow_start = 0;
    const uint32_t gfx = (uint32_t)ctx->r23;
    const uint32_t end = read_word(rdram, gfx);
    // Left where it is, with the caches as it leaves them, if the buffer is full.
    if (deferred_count + (end - start) / 8 > deferred_max) {
        return;
    }
    for (uint32_t cmd = start; cmd < end; cmd += 8) {
        defer(read_word(rdram, cmd), read_word(rdram, cmd + 4));
    }
    write_word(rdram, gfx, start);
    const uint32_t sp = (uint32_t)ctx->r29;
    write_word(rdram, sp + 0x170, (uint32_t)saved_depth_source);
    write_word(rdram, sp + 0x174, (uint32_t)saved_fog);
    write_word(rdram, sp + 0x178, (uint32_t)saved_render_mode);
    write_word(rdram, texture_cache, saved_texture);
    write_word(rdram, tlut_cache, (uint32_t)saved_tlut);
}

extern "C" void rush2049_ghost_arrows_draw(uint8_t* rdram, recomp_context* ctx) {
    if (deferred_count == 0) {
        return;
    }
    const uint32_t gfx = (uint32_t)ctx->r23;
    uint32_t gp = read_word(rdram, gfx);
    for (uint32_t i = 0; i < deferred_count; i++) {
        write_word(rdram, gp, deferred[i * 2]);
        write_word(rdram, gp + 4, deferred[i * 2 + 1]);
        gp += 8;
    }
    deferred_count = 0;
    const uint32_t restore[][2] = {
        { 0xD9FFFFFF, 0x00010000 },  // gsSPSetGeometryMode(G_FOG)
        { 0xE7000000, 0x00000000 },  // gsDPPipeSync()
        { 0xE3000A01, 0x00100000 },  // gsDPSetCycleType(G_CYC_2CYCLE)
    };
    for (const auto& cmd : restore) {
        write_word(rdram, gp, cmd[0]);
        write_word(rdram, gp + 4, cmd[1]);
        gp += 8;
    }
    write_word(rdram, gfx, gp);
    write_word(rdram, texture_cache, 0);
    write_word(rdram, tlut_cache, (uint32_t)-1);
}

