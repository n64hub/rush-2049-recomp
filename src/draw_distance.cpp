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
// Shorter buildings do the same where they stand on higher ground than the road. Driving Track 1's start straight the
// wrong way, toward the hairpin, nine objects come in at once ahead on the left, 900 to 2000 units out, and the tallest
// of them (579 units, its top 700 above the camera, and two of 370 and 383) stand over the low buildings along the road
// (GitHub #267). So Reduced also lets through every object whose top (+0x38 plus +0x60) stands higher above the camera
// than 0.15 of its distance to the box (in x and z, as for the reach), about 8.5 degrees above eye level: what stands
// out on the skyline from far off comes in while it is still small, and what stays below that line is left to the
// stretches. On the camera's path through Track 1's attract race, both ways round, it brings the objects within 50
// degrees of the way the camera moves from 24.3 a frame to 25.9, where Off has 33, and none of them then appears
// standing more than 9 degrees above eye level, where 45 did (20 of them over 17 degrees). The 600-unit rule stays:
// from the track's higher stretches it keeps the towers from farther than this one would.
//
// Neither catches what you look down on. Driving Track 2 the right way, buildings came in at once 1000 to 2500 units
// out, 5 to 20 degrees tall as seen from the camera, with their tops near eye level: they stand on lower ground than the
// road, or the road is up a hill (GitHub #267). So Reduced also lets through every object whose box looks taller than
// `span` (5 degrees) from the camera, from its bottom (+0x38 plus +0x54) to its top, at its distance in x and z. On the
// camera's paths through eleven attract races on Tracks 1 to 6 (a race's camera path read backward for driving the
// wrong way), nothing then comes in looking taller than 5 degrees, where before things up to 24 degrees tall did, and
// Reduced lets through 20.8 objects in view a frame instead of 17.5 (Original 9.6, Off 30.1; backward 22.6 instead of
// 18.5, Off 33.5). Lowering `rise` instead costs more for less (docs/ruled-out.md).
//
// Nor does that catch what is low and wide. Further down Track 2, a long building 1900 units out stood under 4 degrees
// tall from the road but 17 wide, and came in at once as the car drove into the next stretch (GitHub #267). Its height
// barely changes with distance (3 degrees 2500 units out), so `span` would have to drop under that to bring it in
// sooner, for every object on the track. So Reduced also lets through every object
// whose box looks bigger than `area` (40 square degrees) from the camera: its height as above times its width, the
// longer of its x and z extents at the same distance. Along a lap of Track 2 driven the right way, that takes the
// objects in view from 14.3 to 16.6 (Off 24.9), and the biggest that comes in from 165 square degrees to 42. In that lap
// and in the attract race at 1920x1080 it adds about 11% to RT64's draw calls (729 against 656, 678 against 612) and 3%
// to the GPU's time a frame, and on an RTX 2070 the frame rate doesn't move.
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

// Reduced: where the camera is, and the masks of every stretch within reach of it, the objects taller than `tall` units,
// those whose tops stand higher above the camera than `rise` times their distance, those that look taller than `span`
// from the camera and those that look bigger than `area`, while reduced_valid.
constexpr float tall = 600.0f;
constexpr float rise = 0.15f;
constexpr float span = 5.0f * 3.14159265f / 180.0f;
constexpr float area = 40.0f * (3.14159265f / 180.0f) * (3.14159265f / 180.0f);
float camera_x = 0.0f, camera_y = 0.0f, camera_z = 0.0f;
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

static void dev_frame_tick(uint8_t* rdram);
static void dev_log_mask(uint8_t* rdram, const uint32_t* mask);
static void dev_reach_mask(bool first, bool in, uint32_t record, uint32_t row, uint8_t* rdram);
static void dev_note(int n, float high, float wide, bool fixed, float x, float z);
static void dev_counts(uint8_t* rdram);
// DEV ONLY: DEV_AREA_K scales the area limit (and span by its square root) without a rebuild.
static float dev_k(bool root) { static float k = std::getenv("DEV_AREA_K") ? (float)std::atof(std::getenv("DEV_AREA_K")) : 1.0f; return root ? std::sqrt(k) : k; }
extern "C" void rush2049_scenery_camera(uint8_t* rdram, gpr position) {
    dev_frame_tick(rdram);
    const float* p = reinterpret_cast<const float*>(rdram + ((uint32_t)position - 0x80000000));
    camera_x = p[0];
    camera_y = p[1];
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
        uint32_t m[4];
        for (int w = 0; w < 4; w++) m[w] = reach < 0.0f ? 0xFFFFFFFFu : word((uint32_t)ctx->r10 + 4 * w);
        dev_log_mask(rdram, m);
        return;
    }
    uint32_t record = (uint32_t)ctx->r10;
    uint32_t table = record - 16 * stretch;
    for (int w = 0; w < 4; w++) {
        reduced_mask[w] = word(record + 4 * w);
    }
    // The boxes in the game's order, which numbers the records and the objects; each is its centre (+0x34, +0x38,
    // +0x3C) and its extent around it (+0x50 to +0x5C in x, +0x54 to +0x60 in y, +0x58 to +0x64 in z), and +0x44 the
    // next box's index, negative for the last.
    uint32_t box = boxes;
    for (int n = 0; n < 128; n++) {
        float dx = camera_x - value(box + 0x34), dz = camera_z - value(box + 0x3C);
        float ex = std::fmax(0.0f, std::fmax(value(box + 0x50) - dx, dx - value(box + 0x5C)));
        float ez = std::fmax(0.0f, std::fmax(value(box + 0x58) - dz, dz - value(box + 0x64)));
        float d = std::sqrt(ex * ex + ez * ez);
        if (d <= reach) {
            for (int w = 0; w < 4; w++) {
                reduced_mask[w] |= word(table + 16 * n + 4 * w);
            }
        }
        dev_reach_mask(n == 0, d <= reach, record, table + 16 * n, rdram);
        float top = value(box + 0x38) + value(box + 0x60) - camera_y;
        float bottom = value(box + 0x38) + value(box + 0x54) - camera_y;
        float across = std::fmax(d, 1.0f);
        float high = std::atan2(top, across) - std::atan2(bottom, across);
        float wide = 2.0f * std::atan(std::fmax(value(box + 0x5C) - value(box + 0x50),
            value(box + 0x64) - value(box + 0x58)) / 2.0f / across);
        if (value(box + 0x60) - value(box + 0x54) > tall || top > rise * d || high > span * dev_k(true) || high * wide > area * dev_k(false)) {
            reduced_mask[n / 32] |= 1u << (n % 32);
        }
        dev_note(n, high, wide, value(box + 0x60) - value(box + 0x54) > tall || top > rise * d,
            value(box + 0x34), value(box + 0x3C));
        int16_t next = (int16_t)(word((box + 0x44) & ~3u) >> 16);
        if (next < 0) {
            break;
        }
        box = boxes + 0x68 * next;
    }
    reduced_valid = true;
    dev_counts(rdram);
    dev_log_mask(rdram, reduced_mask);
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


// DEV ONLY (local, never committed): attract race on DEV_TRACK (index) / DEV_BACK, held past its 30 s with the view
// pinned (DEV_VIEW), and a log (DEV_LOG) of the camera and of every object let in that was not the frame before.
#include <chrono>
#include <cstdio>
#include <cstdlib>
namespace {
uint8_t* dev_rdram = nullptr;
FILE* dev_log = nullptr;
uint32_t dev_prev[4] = { 0, 0, 0, 0 };
int dev_frame = 0;
uint8_t& dev_u8(uint8_t* rdram, uint32_t a) { return rdram[(a - 0x80000000u) ^ 3]; }
uint32_t& dev_u32(uint8_t* rdram, uint32_t a) { return *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)); }
float& dev_f32(uint8_t* rdram, uint32_t a) { return *reinterpret_cast<float*>(rdram + (a - 0x80000000u)); }
bool dev_attract(uint8_t* rdram) { return (dev_u32(rdram, 0x801174B4) & 0x8) != 0; }
}
extern "C" void rush2049_dev_load_track(uint8_t* rdram) {
    const char* t = std::getenv("DEV_TRACK");
    const char* b = std::getenv("DEV_BACK");
    if (!dev_attract(rdram)) return;
    // The next demo's track: the counter is stepped before the track is taken from it.
    if (t) *reinterpret_cast<int16_t*>(rdram + ((0x80114158u - 0x80000000u) ^ 2)) = (int16_t)(std::atoi(t) - 1);
    if (b && t && dev_u8(rdram, 0x8014978C) == std::atoi(t)) dev_u8(rdram, 0x80152570) = (uint8_t)std::atoi(b);
    std::fprintf(stderr, "DEV load_track track=%d back=%d\n", dev_u8(rdram, 0x8014978C), dev_u8(rdram, 0x80152570));
}
static void dev_frame_tick(uint8_t* rdram) {
    if (!dev_attract(rdram)) return;
    const char* t = std::getenv("DEV_TRACK");
    if (std::getenv("DEV_HOLD") && t && dev_u8(rdram, 0x8014978C) == std::atoi(t)) {
        dev_u32(rdram, 0x8002EB9C) = dev_u32(rdram, 0x8002EB64) + 0x10000000;
        dev_f32(rdram, 0x801106B4) = 1.0e9f;
        // DEV_AB=K: Reduced and Off in turns of K frames, so a pair of captures K frames apart shows what Reduced hides.
        static float dev_saved = 0.0f;
        static int dev_ticks = 0;
        if (std::getenv("DEV_OFF")) {
            scenery_reach.store(-1.0f, std::memory_order_relaxed);
        } else if (const char* ab = std::getenv("DEV_AB")) {
            if (dev_ticks == 0) dev_saved = scenery_reach.load(std::memory_order_relaxed);
            int k = std::atoi(ab);
            scenery_reach.store(((dev_ticks++ / k) & 1) ? -1.0f : dev_saved, std::memory_order_relaxed);
        }
        // DEV_ROCK=T: from race clock T on, the followed car is held where it was then and rocked back and forth along
        // its heading (DEV_ROCK_D units each way, a period of 480 frames), the way a player rolls to and fro over a spot.
        static bool rock_on = false;
        static float rock_p[3], rock_dir[2], rock_o[9];
        static int rock_t = 0;
        if (const char* rk = std::getenv("DEV_ROCK")) {
            uint32_t car = 0x8014A250 + 0x808 * dev_u8(rdram, 0x80150C04);
            if (!rock_on && dev_f32(rdram, 0x801543CC) >= (float)std::atof(rk)) {
                rock_on = true;
                for (int i = 0; i < 3; i++) rock_p[i] = dev_f32(rdram, car + 0x22C + 4 * i);
                for (int i = 0; i < 9; i++) rock_o[i] = dev_f32(rdram, car + 0x2EC + 4 * i);
                float vx = dev_f32(rdram, car + 0x220), vz = dev_f32(rdram, car + 0x228), vl = std::sqrt(vx * vx + vz * vz);
                rock_dir[0] = vl > 0.0f ? vx / vl : 1.0f; rock_dir[1] = vl > 0.0f ? vz / vl : 0.0f;
            }
            if (rock_on) {
                const char* rd = std::getenv("DEV_ROCK_D");
                float s = (rd ? (float)std::atof(rd) : 250.0f) * std::sin(6.2831853f * (rock_t++ % 480) / 480.0f);
                dev_f32(rdram, car + 0x22C) = rock_p[0] + rock_dir[0] * s;
                dev_f32(rdram, car + 0x230) = rock_p[1];
                dev_f32(rdram, car + 0x234) = rock_p[2] + rock_dir[1] * s;
                for (int i = 0; i < 9; i++) dev_f32(rdram, car + 0x2EC + 4 * i) = rock_o[i];
                for (int i = 0; i < 3; i++) {
                    dev_f32(rdram, car + 0x220 + 4 * i) = 0.0f;
                    dev_f32(rdram, car + 0x40 + 4 * i) = 0.0f;
                    dev_f32(rdram, car + 0x4C + 4 * i) = 0.0f;
                }
            }
        }
        const char* v = std::getenv("DEV_VIEW");
        if (v) {
            uint8_t target = dev_u8(rdram, 0x80150C04);
            dev_u8(rdram, 0x80152818 + 0x3B8 * target + 0x35D) = (uint8_t)std::atoi(v);
        }
    }
}
static void dev_log_mask(uint8_t* rdram, const uint32_t* mask) {
    if (!dev_log) {
        const char* p = std::getenv("DEV_LOG");
        if (!p) return;
        dev_log = std::fopen(p, "w");
        if (!dev_log) return;
    }
    if (!dev_attract(rdram)) return;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(dev_log, "F %d %lld clock=%.3f track=%d back=%d off=%d cam=%.0f,%.0f,%.0f\n", dev_frame++, (long long)ms,
        dev_f32(rdram, 0x801543CC), dev_u8(rdram, 0x8014978C), dev_u8(rdram, 0x80152570),
        scenery_reach.load() < 0.0f ? 1 : 0, camera_x, camera_y, camera_z);
    uint32_t boxes = dev_u32(rdram, 0x80149B80);
    uint32_t box = boxes;
    for (int n = 0; n < 128 && boxes; n++) {
        bool now = (mask[n / 32] >> (n % 32)) & 1, before = (dev_prev[n / 32] >> (n % 32)) & 1;
        if (now && !before) {
            float dx = camera_x - dev_f32(rdram, box + 0x34), dz = camera_z - dev_f32(rdram, box + 0x3C);
            std::fprintf(dev_log, "  IN %d d=%.0f at=%.0f,%.0f,%.0f h=%.0f w=%.0f,%.0f\n", n, std::sqrt(dx * dx + dz * dz),
                dev_f32(rdram, box + 0x34), dev_f32(rdram, box + 0x38), dev_f32(rdram, box + 0x3C),
                dev_f32(rdram, box + 0x60) - dev_f32(rdram, box + 0x54), dev_f32(rdram, box + 0x5C) - dev_f32(rdram, box + 0x50),
                dev_f32(rdram, box + 0x64) - dev_f32(rdram, box + 0x58));
        }
        int16_t next = (int16_t)(dev_u32(rdram, (box + 0x44) & ~3u) >> 16);
        if (next < 0) break;
        box = boxes + 0x68 * next;
    }
    std::memcpy(dev_prev, mask, sizeof dev_prev);
    std::fflush(dev_log);
}
// DEV ONLY: objects within 50 degrees of the camera's heading that each rule set would let through, a frame.
namespace {
uint32_t dev_rec[4], dev_tab[4];
float dev_high[128], dev_wide[128], dev_x[128], dev_z[128];
bool dev_fix[128];
int dev_n = 0;
float dev_hist[32][2];
int dev_hn = 0;
}
static void dev_reach_mask(bool first, bool in, uint32_t record, uint32_t row, uint8_t* rdram) {
    if (first) {
        for (int w = 0; w < 4; w++) dev_tab[w] = dev_rec[w] = dev_u32(rdram, record + 4 * w);
        dev_n = 0;
    }
    if (in) for (int w = 0; w < 4; w++) dev_tab[w] |= dev_u32(rdram, row + 4 * w);
}
static void dev_note(int n, float high, float wide, bool fixed, float x, float z) {
    dev_high[n] = high; dev_wide[n] = wide; dev_fix[n] = fixed; dev_x[n] = x; dev_z[n] = z;
    if (n + 1 > dev_n) dev_n = n + 1;
}
static void dev_counts(uint8_t* rdram) {
    if (!dev_log || !dev_attract(rdram)) return;
    float* old = dev_hist[(dev_hn + 1) % 32];
    float hx = camera_x - old[0], hz = camera_z - old[1];
    dev_hist[dev_hn % 32][0] = camera_x; dev_hist[dev_hn % 32][1] = camera_z; dev_hn++;
    if (dev_hn < 32 || hx * hx + hz * hz < 100.0f) return;
    const float ks[4] = { 1.0f, 0.5f, 0.25f, 0.125f };
    int orig = 0, tab = 0, all = 0, red[4] = { 0, 0, 0, 0 };
    float hl = std::sqrt(hx * hx + hz * hz);
    for (int n = 0; n < dev_n; n++) {
        float ox = dev_x[n] - camera_x, oz = dev_z[n] - camera_z, ol = std::sqrt(ox * ox + oz * oz);
        if (ol > 1.0f && (ox * hx + oz * hz) / (ol * hl) < std::cos(50.0f * 3.14159265f / 180.0f)) continue;
        all++;
        bool o = (dev_rec[n / 32] >> (n % 32)) & 1, t = (dev_tab[n / 32] >> (n % 32)) & 1;
        orig += o; tab += t;
        for (int k = 0; k < 4; k++)
            red[k] += t || dev_fix[n] || dev_high[n] > span * std::sqrt(ks[k]) || dev_high[n] * dev_wide[n] > area * ks[k];
    }
    std::fprintf(dev_log, "C %d %d %d %d %d %d %d\n", orig, tab, red[0], red[1], red[2], red[3], all);
}
