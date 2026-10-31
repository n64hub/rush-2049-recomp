// Draw Distance and Level of Detail (Graphics tab, config.cpp): the game's distance limits as multiples of its own.
// Scenery Culling, beside them: the game's table of which scenery can be seen from where.
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
// Scenery Culling, beside them: each track has a table of which scenery can be seen from where. func_8009EBC0, called
// by func_8009F058 for each camera a frame, finds the stretch of road the camera is in (a list of boxes, 0x68 bytes
// each, from the pointer at 0x80149B80) and takes that stretch's 16-byte record from the track's table (picked by the
// track index at 0x8014978C; Track 1's starts at 0x8011B898): a 128-bit mask of the track's scenery objects (their
// count is the byte at 0x8011E748 + the track; Track 1 has 117), and every object whose bit is clear gets the disable
// bit (bit 31) in its node's flags. It hides 79-100 of Track 1's objects from each stretch, and not only what can't be
// seen: in the attract race's opening shot of Track 1 a tall building stands over the trees on the right with the
// table off and is sky with it on (the race slowed twelvefold and the table switched every four frames). As the
// camera crosses from one stretch into the next, what the new mask lets through appears at once, at any distance,
// which is what a player reported on Track 1 (GitHub #267): scenery close to the car coming and going as they roll
// back and forth over one spot. Off (us.toml, at 0x8009EFEC) treats every bit as set, so every object is drawn from
// every stretch and the limits alone decide. While the byte at 0x8014061A is set, the function disables every object
// whatever its bit; that is kept. In a browser the option starts at Original: Off costs the page a third of its frames
// (docs/web.md).
//
// Reduced, between them, also lets through what every stretch within 500 units of the camera (in x and z, to the edge
// of its box) lets through, so what pops in as you cross into a stretch was let in 500 units before you got there. The
// boxes are the scenery objects' own: box n's centre is where object n stands (all 117 of Track 1's), so the stretch
// the camera is in is the object it is inside, mostly a piece of road, and 109 of Track 1's records let their own
// object through. On the camera's path through attract races on Tracks 1 and 2, with each
// object put at its own stretch's box, Original let 12 and 3 objects in within 300 units of the camera and Reduced
// none, while it let through 46 and 33 objects a frame where Original lets through 26 and 22 and Off all 117. The
// boxes are the list func_8009EBC0 walks (a box's centre at +0x34, +0x3C, its extent around that from +0x50 to +0x5C
// in x and +0x58 to +0x64 in z, +0x44 the next box's index), whose order numbers the records, and the camera's
// position is the one it is given (a1, us.toml); the record it picked is t2 at 0x8009EF88. The list's order is not
// the road's: a car crosses three to six boxes' numbers at a time, and the list jumps between parts of the track, so
// "the next few stretches" is not a fixed distance along the road. Where no box holds the camera, or the table is off,
// Reduced leaves the mask alone. In the attract race at 1600x1200, with the view test below, it draws 570 calls a
// frame where Original draws 490 and Off 840 (README, Performance).
//
// The table is made for driving the right way. Driving the wrong way the tallest buildings stand over everything in
// front of them from much farther than 500 units: at the foot of Track 1's downhill, where it meets the loop, looking
// back the way the track comes, five of the skyline's towers 2000 to 3100 units out are let through by no stretch within
// 500 units, only by stretches 1000 to 1600 away, and a player saw them pop in driving there backward (GitHub #267). So
// Reduced also lets through every object whose box (+0x54 to +0x60 in y) is more than 600 units tall, wherever the
// camera is, and the view test below skips those out of view. Track 1 has 16, its towers. At that spot (1600x900)
// Reduced then draws 1108 calls a frame where Off draws 1366 and Original 639, and its picture differs from Off's in
// under a thousand pixels instead of 16000; in 16 views around Track 1, most of them looking back, what is still
// missing beside Off is small and far. In the attract race at 1600x1200 the towers cost no calls past the noise between
// runs (449 and 471 a frame against 469 and 526 without them). A longer reach would take the towers in too, but costs
// nearly what Off does: it takes about 1500 units, with which 36 objects are in view on average where the towers bring
// Reduced from 27 to 30 and Off draws 43 (each object's box against the view, from the camera's path through Track 1).
// Why Off isn't cut down the same way is in docs/ruled-out.md.
//
// Besides these limits and that table, func_8009DD88 skips a node only by its per-camera hidden bit (0x100 << camera
// in its flags, func_8009DD18: hidden unless a child is visible) and its disable bit. The game sets and clears them
// with func_8008AE8C and func_8008B0D8, and in five minutes of attract races every such call was on a car's parts
// (wheels, its body in the in-car view) or the pool of sparks and smoke func_8008EA10 places around the cars: about 300
// nodes, none of them scenery. Nor does the game test a node against the view on the CPU: the node walker, the model
// draw and the matrix builders compare only distances. The walker leaves that to the RSP: each node's list loads the
// node's bounding box (8 vertices) and a G_CULLDL, which ends the list when the box lies off one side of the view.
// Upstream RT64 treats it as a no-op and draws every node in reach; runtime-patches/rt64-0008 runs it, against the view as RT64 widens it and a quarter more each way, so at a wide aspect
// ratio nothing is missing at the sides. Through two attract races, at 16:9 and 4:3, none of the 80 million triangles
// it skipped would have been in view without that quarter. In the attract race at 1600x1200 it takes Original from
// 670 draws a frame to 490 and Off from 1960 to 840, and Off from 43 fps to 58 on a busy 6-core PC, where RT64's
// thread was the limit (README, Performance).

#include <atomic>
#include <cmath>
#include <cstring>

#include "rush2049_game.h"
#include "rush2049_hooks.h"

namespace {

// Set from the settings menu, read on the game thread. They start at the options' defaults (config.cpp): with no
// settings file yet, the first launch saves the defaults and calls no option's change callback.
std::atomic<float> distance_scale{ 4.0f };
std::atomic<float> detail_scale{ 4.0f };
#ifdef __EMSCRIPTEN__
std::atomic<float> scenery_reach{ 0.0f };
#else
std::atomic<float> scenery_reach{ -1.0f };
#endif

// Reduced: where the camera is (x, z), and the masks of every stretch within reach of it and the objects taller than
// `tall` units, while reduced_valid.
constexpr float tall = 600.0f;
float camera_x = 0.0f, camera_z = 0.0f;
uint32_t reduced_mask[4];
bool reduced_valid = false;

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

void rush2049::set_scenery_culling(float reach) {
    scenery_reach.store(reach, std::memory_order_relaxed);
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

extern "C" void rush2049_scenery_camera(uint8_t* rdram, gpr position) {
    const float* p = reinterpret_cast<const float*>(rdram + ((uint32_t)position - 0x80000000));
    camera_x = p[0];
    camera_z = p[2];
}

extern "C" void rush2049_scenery_stretch(uint8_t* rdram, recomp_context* ctx) {
    reduced_valid = false;
    float reach = scenery_reach.load(std::memory_order_relaxed);
    auto word = [rdram](uint32_t addr) { return *reinterpret_cast<const uint32_t*>(rdram + (addr - 0x80000000)); };
    auto value = [&word](uint32_t addr) { uint32_t w = word(addr); float f; std::memcpy(&f, &w, 4); return f; };
    int32_t stretch = (int32_t)word((uint32_t)ctx->r29 + 0x78);
    uint32_t boxes = word(0x80149B80);
    if (reach <= 0.0f || stretch < 0 || boxes == 0) {
        return;
    }
    uint32_t record = (uint32_t)ctx->r10;
    uint32_t table = record - 16 * stretch;
    for (int w = 0; w < 4; w++) {
        reduced_mask[w] = word(record + 4 * w);
    }
    // The boxes in the game's order, which numbers the records and the objects; each is its centre (+0x34, +0x3C) and
    // its extent around it (+0x50 to +0x5C in x, +0x54 to +0x60 in y, +0x58 to +0x64 in z), and +0x44 the next box's
    // index, negative for the last.
    uint32_t box = boxes;
    for (int n = 0; n < 128; n++) {
        float dx = camera_x - value(box + 0x34), dz = camera_z - value(box + 0x3C);
        float ex = std::fmax(0.0f, std::fmax(value(box + 0x50) - dx, dx - value(box + 0x5C)));
        float ez = std::fmax(0.0f, std::fmax(value(box + 0x58) - dz, dz - value(box + 0x64)));
        if (ex * ex + ez * ez <= reach * reach) {
            for (int w = 0; w < 4; w++) {
                reduced_mask[w] |= word(table + 16 * n + 4 * w);
            }
        }
        if (value(box + 0x60) - value(box + 0x54) > tall) {
            reduced_mask[n / 32] |= 1u << (n % 32);
        }
        int16_t next = (int16_t)(word((box + 0x44) & ~3u) >> 16);
        if (next < 0) {
            break;
        }
        box = boxes + 0x68 * next;
    }
    reduced_valid = true;
}

extern "C" gpr rush2049_scenery_mask(gpr mask, gpr offset) {
    float reach = scenery_reach.load(std::memory_order_relaxed);
    if (reach < 0.0f) {
        return (gpr)(int32_t)-1;
    }
    if (reach > 0.0f && reduced_valid) {
        return (gpr)(int32_t)reduced_mask[((uint32_t)offset >> 2) & 3];
    }
    return mask;
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

