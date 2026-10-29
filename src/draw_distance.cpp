// Draw Distance and Level of Detail (Graphics tab, config.cpp): the game's distance limits as multiples of its own.
//
// The limits, all hooked in us.toml. Draw Distance scales the first four, Level of Detail the last:
//   Far plane      the far plane (camera +0x3C, times 16 in world units: 32000 in a race) as func_8009F058 builds the
//       projection, and the cull distance (0x80151AA0) func_8009DD88 tests nodes against, which the game only sets
//       below its 2000 sentinel for split screen.
//       The game draws some text and icons at a depth it works out itself, not from a vertex: func_8008A644 sets the
//       RDP's prim depth to a 15-bit screen depth, from a distance (func_800B65B4) or a table of distances and depths
//       (func_800F6928), worked out for its own projection. A menu draws each label just in front of its button that
//       way, and the race HUD its text. A perspective projection's depth is F/(F-n) * (1 - n/z), so a far plane s times
//       farther moves every depth by one factor, (1 - n/F) / (1 - n/sF), about 0.999 at 4x: enough to put the buttons in
//       front of their labels, which then go missing (the profile and Controls screens, most of a run) or flicker. So
//       each prim depth is multiplied by that factor, from the near and far planes of the projection built last.
//   Matrix limit   the 2048-unit test of the node matrix builders (func_8009D99C, func_8009D45C): a node whose offset
//       from the camera is 2048 or more on any axis gets no matrix and isn't drawn, as s15.16 can't hold the
//       translation (2048 * 16 = 32768). Here a farther node's offset is divided by a power of two before the test,
//       and the rest of the matrix, w included, by the same number after it is written: a homogeneous matrix divided
//       through puts every vertex in the same place on screen, so nothing is added to the display list.
//   Depth clamp    the 1600-unit clamp of the far node matrix builder (func_8009D708), which the node walker uses for
//       distant flat scenery (node flag 0x400000), such as the boards painted with trees and buildings across Track 1's
//       bay: past 1600 from the camera, a node's offset and its size are both multiplied by 1600 over its distance. It
//       stays where it was on screen, but its depth is that of a node 1600 out, so it covers whatever lies between 1600
//       and where it really is, and two clamped boards lose their order. As the camera comes closer such a board keeps
//       that depth until it is within 1600, and then comes closer with it. The clamp keeps the boards inside the far
//       plane (Track 1's reach 2115, past its 2000). It is scaled like the far plane, and a node it leaves 2048 or more
//       out on an axis is fitted into s15.16 the way the matrix limit's is, by dividing its offset, the scale the game
//       multiplies the matrix by, and w, before they are written.
//   LOD cutoff     the LOD cutoff in func_8009C8F0: past its last level's distance times the node's scale a model is
//       skipped.
//   LOD detail     the LOD level choice there: the distances inside which a model is drawn at a more detailed level.
//       Highest makes them unreachable, so every model is drawn at its most detailed level.
//
// A node's origin is what the limits test, and a large one (a building, a stretch of road) can sit past them while its
// near end is in view: that end pops in when the origin comes within reach. A player found two such places on Track 1:
// a building behind the walkway over the road past the first turn, which the matrix limit at 4x shows a little sooner,
// and the curved road before the tunnel, drawn at a coarse level until close at Original and round at High. Most of
// what the limits drop is out of view anyway (behind buildings, hills or the camera): with every limit at 4x, paused
// frames at about 35 spots on tracks 1-8 and in split screen are the same to the pixel.
//
// Not a limit: the forested hill across the bay from Track 1's start is a few flat boards painted with trees, all drawn
// in every frame, 16000-31000 units out, the farthest through the depth clamp. Each board's top edge is one straight
// line across the painted trees, and as the near boards slide over the far ones the treetops seem to come and go. A
// recording of an N64 shows the same.
//
// No zones either: nothing hides scenery by where the car is on the track. Besides these limits func_8009DD88 skips a
// node only by its per-camera hidden bit (0x100 << camera in its flags, func_8009DD18: hidden unless a child is
// visible) and its disable bit (bit 31). The game sets and clears them with func_8008AE8C and func_8008B0D8, and in
// five minutes of attract races every such call was on a car's parts (wheels, its body in the in-car view) or the
// pool of sparks and smoke func_8008EA10 places around the cars: about 300 nodes, none of them scenery.

#include <atomic>
#include <cmath>

#include "rush2049_game.h"
#include "rush2049_hooks.h"

namespace {

// Set from the settings menu, read on the game thread.
std::atomic<float> distance_scale{ 1.0f };
std::atomic<float> detail_scale{ 1.0f };

// What func_8009D99C or func_8009D45C divided the node's offset by, from the start of the call to its end.
int matrix_divisor = 1;

// What the last projection's far plane moved a depth by.
float prim_depth_scale = 1.0f;

float distance() {
    return distance_scale.load(std::memory_order_relaxed);
}

} // namespace

void rush2049::set_draw_distance(float scale) {
    distance_scale.store(scale, std::memory_order_relaxed);
}

void rush2049::set_level_of_detail(float scale) {
    detail_scale.store(scale, std::memory_order_relaxed);
}

extern "C" float rush2049_draw_distance_lod(float limit) {
    return limit * distance();
}

extern "C" float rush2049_draw_distance_level(float switch_distance) {
    return switch_distance * detail_scale.load(std::memory_order_relaxed);
}

extern "C" float rush2049_draw_distance_far(float far, float near) {
    float s = distance();
    prim_depth_scale = (1.0f - near / far) / (1.0f - near / (far * s));
    return far * s;
}

extern "C" gpr rush2049_draw_distance_prim_depth(gpr z) {
    uint32_t depth = (uint32_t)z & 0xFFFF;
    if (depth > 0x7FFF) {
        return z;
    }
    return (gpr)std::lround(depth * prim_depth_scale);
}

extern "C" float rush2049_draw_distance_cull(float cull) {
    return cull * distance();
}

extern "C" void rush2049_far_matrix_begin(recomp_context* ctx) {
    matrix_divisor = 1;
    float d = std::fmax(std::fabs(ctx->f0.fl), std::fmax(std::fabs(ctx->f2.fl), std::fabs(ctx->f12.fl)));
    if (d < 2048.0f || d >= 2048.0f * distance()) {
        return;
    }
    while (d >= 2048.0f * matrix_divisor) {
        matrix_divisor *= 2;
    }
    float s = 1.0f / matrix_divisor;
    ctx->f0.fl *= s;
    ctx->f2.fl *= s;
    ctx->f12.fl *= s;
}

extern "C" float rush2049_far_clamp_distance(float d) {
    return d / distance();
}

extern "C" void rush2049_far_clamp_fit(recomp_context* ctx) {
    float d = std::fmax(std::fabs(ctx->f2.fl), std::fmax(std::fabs(ctx->f12.fl), std::fabs(ctx->f14.fl)));
    if (d < 2048.0f) {
        return;
    }
    int divisor = 2;
    while (d >= 2048.0f * divisor) {
        divisor *= 2;
    }
    float s = 1.0f / divisor;
    ctx->f2.fl *= s;
    ctx->f12.fl *= s;
    ctx->f14.fl *= s;
    ctx->f16.fl *= s;
    ctx->f18.fl *= s;
}

// The matrix is N64 s15.16: the 16 whole parts in the first 32 bytes, then the 16 fractions, two to a word. The
// translation (elements 12-14) was built from the divided offset already.
extern "C" void rush2049_far_matrix_end(uint8_t* rdram, gpr matrix) {
    if (matrix_divisor == 1) {
        return;
    }
    uint32_t* words = reinterpret_cast<uint32_t*>(rdram + ((uint32_t)matrix - 0x80000000));
    for (int i = 0; i < 16; i++) {
        if (i >= 12 && i <= 14) {
            continue;
        }
        uint32_t& whole = words[i / 2];
        uint32_t& frac = words[8 + i / 2];
        int shift = (i & 1) ? 0 : 16;
        int32_t value = (int32_t)((((whole >> shift) & 0xFFFF) << 16) | ((frac >> shift) & 0xFFFF));
        value /= matrix_divisor;
        whole = (whole & ~(0xFFFFu << shift)) | (((uint32_t)value >> 16) << shift);
        frac = (frac & ~(0xFFFFu << shift)) | (((uint32_t)value & 0xFFFF) << shift);
    }
}
