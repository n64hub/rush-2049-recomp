// Draw Distance's node matrix hooks (src/draw_distance.cpp) checks, run by CI: at Original nothing changes; a node
// within the setting's reach has its offset divided by a power of two that brings it under the game's 2048 test, and
// every element of its s15.16 matrix but the translation divided by the same number; a node past the reach is left to
// the game's own test. The far builder's depth clamp reaches 1600 times the setting, and an offset it leaves 2048 or more
// out is divided down with the rotation's scale and w. A prim depth moves as the far plane moves a vertex's depth.
// Scenery Culling's Reduced ORs a stretch's visibility mask with its neighbours', wrapping at the ends.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rush2049_game.h"
#include "rush2049_hooks.h"

namespace {

int failures = 0;

void check(bool ok, const char* what, double value) {
    printf("%s %s (%g)\n", ok ? "ok  " : "FAIL", what, value);
    if (!ok) {
        failures++;
    }
}

constexpr uint32_t matrix_addr = 0x80001000;

// N64 s15.16 layout: the 16 whole parts two to a word, then the 16 fractions.
void write_matrix(uint8_t* rdram, const int32_t (&m)[16]) {
    uint32_t* words = reinterpret_cast<uint32_t*>(rdram + (matrix_addr - 0x80000000));
    for (int i = 0; i < 8; i++) {
        words[i] = ((uint32_t)m[2 * i] & 0xFFFF0000u) | ((uint32_t)m[2 * i + 1] >> 16);
        words[8 + i] = ((uint32_t)m[2 * i] << 16) | ((uint32_t)m[2 * i + 1] & 0xFFFFu);
    }
}

int32_t read_element(const uint8_t* rdram, int i) {
    const uint32_t* words = reinterpret_cast<const uint32_t*>(rdram + (matrix_addr - 0x80000000));
    int shift = (i & 1) ? 0 : 16;
    return (int32_t)((((words[i / 2] >> shift) & 0xFFFF) << 16) | ((words[8 + i / 2] >> shift) & 0xFFFF));
}

// Runs the hooks around a "matrix builder" that writes `m` for an offset (x, y, z); returns the offset it built from.
void build(uint8_t* rdram, float x, float y, float z, const int32_t (&m)[16], float out[3]) {
    recomp_context ctx{};
    ctx.f0.fl = x;
    ctx.f2.fl = y;
    ctx.f12.fl = z;
    rush2049_far_matrix_begin(&ctx);
    out[0] = ctx.f0.fl;
    out[1] = ctx.f2.fl;
    out[2] = ctx.f12.fl;
    write_matrix(rdram, m);
    rush2049_far_matrix_end(rdram, (gpr)(int32_t)matrix_addr);
}

// The far builder's clamp test (f8 < f0, 1600 < distance) as the hooked code sees it.
bool far_clamped(float d) {
    return 1600.0f < rush2049_far_clamp_distance(d);
}

// Runs the fit hook on an offset (x, y, z) with the rotation's scale f16 and w f18; returns the five after it.
void fit(float x, float y, float z, float scale, float w, float out[5]) {
    recomp_context ctx{};
    ctx.f2.fl = x;
    ctx.f12.fl = y;
    ctx.f14.fl = z;
    ctx.f16.fl = scale;
    ctx.f18.fl = w;
    rush2049_far_clamp_fit(&ctx);
    out[0] = ctx.f2.fl;
    out[1] = ctx.f12.fl;
    out[2] = ctx.f14.fl;
    out[3] = ctx.f16.fl;
    out[4] = ctx.f18.fl;
}

} // namespace

int main() {
    std::vector<uint8_t> rdram(0x2000);
    // A rotation and scale with fractions and negative elements, a translation, and w 1.
    const int32_t m[16] = {
        0x00012345, -0x00008000, 0x00000003, 0,
        0x00007FFF, 0x00010000, -0x00012345, 0,
        -0x00000001, 0x00020000, 0x0000ABCD, 0,
        0x01230000, -0x02340000, 0x00450000, 0x00010000,
    };
    float offset[3];

    rush2049::set_draw_distance(1.0f);
    build(rdram.data(), 5000.0f, 100.0f, -3000.0f, m, offset);
    check(offset[0] == 5000.0f && offset[2] == -3000.0f, "Original: the offset is left for the game to reject", offset[0]);
    bool same = true;
    for (int i = 0; i < 16; i++) {
        same = same && read_element(rdram.data(), i) == m[i];
    }
    check(same, "Original: the matrix is untouched", 0);

    rush2049::set_draw_distance(4.0f);
    build(rdram.data(), 5000.0f, 100.0f, -3000.0f, m, offset);
    check(offset[0] == 1250.0f && offset[1] == 25.0f && offset[2] == -750.0f, "4x: a 5000 offset is divided by 4",
        offset[0]);
    bool divided = true;
    for (int i = 0; i < 16; i++) {
        int32_t want = (i >= 12 && i <= 14) ? m[i] : m[i] / 4;
        divided = divided && read_element(rdram.data(), i) == want;
    }
    check(divided, "4x: every element but the translation is divided by 4, w included", read_element(rdram.data(), 15));

    build(rdram.data(), 100.0f, 2047.0f, 0.0f, m, offset);
    check(offset[1] == 2047.0f && read_element(rdram.data(), 0) == m[0], "4x: a node the game draws anyway is untouched",
        offset[1]);

    build(rdram.data(), 0.0f, -9000.0f, 0.0f, m, offset);
    check(offset[1] == -9000.0f && read_element(rdram.data(), 0) == m[0], "4x: a node past 8192 is left to the game",
        offset[1]);

    rush2049::set_draw_distance(16.0f);
    build(rdram.data(), 0.0f, 0.0f, -30000.0f, m, offset);
    check(offset[2] == -1875.0f && read_element(rdram.data(), 15) == m[15] / 16, "16x: a 30000 offset is divided by 16",
        offset[2]);

    float f[5];
    rush2049::set_draw_distance(1.0f);
    check(far_clamped(1601.0f) && !far_clamped(1600.0f), "Original: the depth clamp is at 1600",
        rush2049_far_clamp_distance(1601.0f));
    fit(-1600.0f, 50.0f, 0.0f, 0.5f, 1.0f, f);
    check(f[0] == -1600.0f && f[3] == 0.5f && f[4] == 1.0f, "Original: a clamped node is untouched", f[0]);

    rush2049::set_draw_distance(4.0f);
    check(!far_clamped(6400.0f) && far_clamped(6401.0f), "4x: the depth clamp is at 6400",
        rush2049_far_clamp_distance(6401.0f));
    fit(5000.0f, 100.0f, -3000.0f, 1.0f, 2.0f, f);
    check(f[0] == 1250.0f && f[1] == 25.0f && f[2] == -750.0f && f[3] == 0.25f && f[4] == 0.5f,
        "4x: a 5000 offset is divided by 4 with the scale and w", f[0]);
    fit(2047.0f, 0.0f, 0.0f, 1.0f, 1.0f, f);
    check(f[0] == 2047.0f && f[3] == 1.0f, "4x: an offset s15.16 holds is untouched", f[0]);

    rush2049::set_draw_distance(16.0f);
    fit(0.0f, 0.0f, -25600.0f, 0.8f, 1.0f, f);
    check(f[2] == -1600.0f && f[3] == 0.05f && f[4] == 0.0625f, "16x: a 25600 offset is divided by 16", f[2]);

    // A prim depth worked out for the game's projection (near 2.5, far 2000) lands where the projection Draw Distance
    // builds puts a vertex at the same distance.
    auto depth = [](float near, float far, float z) { return far / (far - near) * (1.0f - near / z) * 0x7FFF; };
    rush2049::set_draw_distance(1.0f);
    check(rush2049_draw_distance_far(2000.0f, 2.5f) == 2000.0f &&
        rush2049_draw_distance_prim_depth(0x7E2A) == 0x7E2A, "Original: a prim depth is untouched",
        (float)rush2049_draw_distance_prim_depth(0x7E2A));
    rush2049::set_draw_distance(4.0f);
    rush2049_draw_distance_far(2000.0f, 2.5f);
    float moved = (float)rush2049_draw_distance_prim_depth((gpr)std::lround(depth(2.5f, 2000.0f, 150.0f)));
    check(std::fabs(moved - depth(2.5f, 8000.0f, 150.0f)) <= 1.0f, "4x: a prim depth moves with the far plane", moved);
    check(rush2049_draw_distance_prim_depth(0xFFFF) == 0xFFFF, "4x: a depth past 15 bits is left alone",
        (float)rush2049_draw_distance_prim_depth(0xFFFF));

    // Scenery Culling: a table of 6 stretches whose records each let through only their own object (bit i of the first
    // word), and the stack func_8009EBC0 leaves at 0x8009EF88: the stretch at 0x78, the last stretch's at 0x84.
    constexpr uint32_t table_addr = 0x80001400, stack_addr = 0x80001800;
    auto ram_word = [&rdram](uint32_t addr) -> uint32_t& {
        return *reinterpret_cast<uint32_t*>(rdram.data() + (addr - 0x80000000));
    };
    for (int i = 0; i < 6; i++) {
        for (int w = 0; w < 4; w++) {
            ram_word(table_addr + 16 * i + 4 * w) = w == 0 ? 1u << i : 0;
        }
    }
    // The first word of the mask the game then tests, or 0xDEAD if its second word is not 0.
    auto shown = [&](int reach, int32_t stretch, int32_t last) {
        rush2049::set_scenery_culling(reach);
        recomp_context ctx{};
        ctx.r29 = (gpr)(int32_t)stack_addr;
        ctx.r10 = (gpr)(int32_t)(table_addr + 16 * std::max(stretch, 0));
        ram_word(stack_addr + 0x78) = (uint32_t)stretch;
        ram_word(stack_addr + 0x84) = (uint32_t)last;
        rush2049_scenery_stretch(rdram.data(), &ctx);
        uint32_t first = (uint32_t)rush2049_scenery_mask((gpr)(int32_t)ram_word((uint32_t)ctx.r10), 0);
        uint32_t second = (uint32_t)rush2049_scenery_mask(0, 4);
        return second ? 0xDEADu : first;
    };
    check(shown(0, 2, 5) == 0x04, "Original: a stretch's own mask", shown(0, 2, 5));
    check(shown(2, 2, 5) == 0x1F, "Reduced: two stretches either side", shown(2, 2, 5));
    check(shown(2, 0, 5) == 0x37, "Reduced: the ends wrap", shown(2, 0, 5));
    check(shown(2, -1, 5) == 0x01, "Reduced: no stretch holds the camera, the game's mask", shown(2, -1, 5));
    check(shown(2, 0, 0) == 0x01, "Reduced: one stretch, its own mask", shown(2, 0, 0));
    check(shown(2, 1, 2) == 0x07, "Reduced: three stretches, each once", shown(2, 1, 2));
    rush2049::set_scenery_culling(-1);
    check(rush2049_scenery_mask(0, 0) == (gpr)(int32_t)-1, "Off: every bit set", 0);

    printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
