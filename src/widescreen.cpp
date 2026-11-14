// Widescreen race HUD. RT64 widens the 3D view, but the game draws its HUD as texture rectangles at fixed 4:3
// positions, so at 16:9 the speedometer and the lap/timer/minimap block sit inside the 4:3 area. Every texture
// rectangle the game draws goes through func_80087110 (x0, y0, x1, y1 in screen pixels), which appends a
// G_TEXRECT + G_RDPHALF_1 + G_RDPHALF_2 triple at the display list pointer in 0x80149438. When the rectangle belongs
// to the left or right HUD block, a hook at the function's exit rewrites that triple in place as RT64's extended
// texture rectangle (the same 24 bytes) with a left or right origin, so it anchors to that edge of the window. RT64's
// HUD Placement option (Graphics tab) decides how far it moves, and at Original none of this runs, so the game's
// display lists reach RT64 as the game wrote them. Nothing is inserted: callers keep their own copies of the display
// list pointer across the call, so extra commands would be overwritten.
//
// Menus use the same function, so this only applies during a race, which is when the race timer is drawn
// (func_8010761C). RT64 only reads extended commands after a gEXEnable in the same display list, which every frame's
// list starts with (src/frame_list.cpp). Should that list be in the game's own slot, its first command, setting
// segment 0 to 0, which is already its value, becomes gEXEnable while the HUD or the menus move.
//
// The two spinning coins beside the right-hand counters are 3D models, drawn among the world's objects with a matrix
// that keeps them in front of the camera. They're the only objects whose matrix has 10 in its bottom-right corner.
// Each is a top-level matrix + display list call + pop, and that triple is replaced by a call to a list of our own
// (in the recomp heap, above the game's 8 MB, so RT64 reads it in extended RDRAM mode) that draws the coin under a
// viewport anchored to the same edge as the counters. RT64 slides an anchored viewport as far as it slides the HUD.
//
// Split screen. RT64 only widens a viewport that spans the screen, so the 2- and 4-player views stay 4:3. Each view
// is a top-level viewport + pipe sync + scissor, and with "Split-Screen Aspect Ratio" at Expand that triple becomes a
// call to a list that sets the same viewport with its left and right edges anchored to origins (a patch to RT64,
// runtime-patches/rt64-0001-viewport-edge-origins.patch) and the scissor anchored the same way. An edge in the left
// quarter of the screen follows the window's left edge, one in the right quarter its right edge, and one in between
// the centre, so the two 2-player views reach both sides of the window and the four 4-player views meet in the middle.
// How far the origins move is HUD Placement's, so at 16:9 a wider window keeps bars beside the views; at Full Window,
// each frame widened here tells RT64 to move them to the window's edges (runtime-patches/rt64-0012) and the rest keep
// the player's placement.
// RT64 widens the projection with the view. The HUD moves with the views: a rectangle in a view anchors to the view's
// left edge, right edge or centre, whichever it's nearest, and a rectangle outside the views to the nearest view edge.
// Text is a rectangle a letter, so rectangles drawn one after another along one line (a word, a line of the pause
// menu, a picture in strips) are anchored together, by where the whole line sits; letter by letter, a word reaching
// across a third of the view would come apart.
// The borders between views are fill rectangles followed by pipe syncs at the start of the frame, after the frame's
// clear, a fill across the whole screen in the game's background colour. The clear stays the game's own, which RT64
// widens to the window, so the window past where HUD Placement stops the views (16:9's edges by default) is cleared
// every frame, as at Original, and doesn't keep the last menu's picture there. The 4-player
// score boxes and the pause menu's panels and their border lines are fill rectangles in the HUD's list; each fill +
// sync pair is rewritten in place as RT64's extended fill rectangle, anchored the same way, and fills that touch are a
// box, anchored by where the whole box sits. A line drawn after a box, with its middle in it, is anchored as the box,
// so a menu's text stays in its panel: the pause menu's NO, the end of its DEFAULTS and the scoreboard's DEATHS sit
// in the right third of a view, and by where they sit would follow its right edge out of their centred panel. The
// race HUD the pause menu covers is drawn before the menu, and keeps its own anchoring. So lines are anchored when the
// list is submitted, once its boxes are known.
//
// Menus. Their background is a 4:3 picture drawn as texture rectangles, and the menu sits on it, so the menu stays
// where it is and the picture is extended: with "Menu Aspect Ratio" at Expand, each strip of it at the screen's left
// or right edge becomes a call to a list that draws the strip and its mirror image across the edge, so the side bars
// show the circuit-board pattern carried on rather than black. The strips' scissor is anchored to the extended GBI's
// left and right origins, which HUD Placement stops at 16:9's edges by default, so a menu frame tells RT64 to put them
// at the window's (runtime-patches/rt64-0012), and a 21:9 window gets the pattern rather than bars past 16:9. A car
// select's panels each draw their car under a viewport of their own, laid out like split screen, so a frame that draws
// the background is a menu, not split screen.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "rush2049_game.h"
#include "rush2049_hooks.h"
#include "ultramodern/config.hpp"

#define F3DEX_GBI_2
#include "../lib/rt64/include/rt64_extended_gbi.h"

namespace {

constexpr uint32_t sched_addr = 0x8002E8E8;     // the game's scheduler; +0x27C counts retraces
constexpr uint32_t dl_ptr_addr = 0x80149438;    // where func_80087110 appends commands

// The 4:3 race HUD: the speedometer and gauges end by x 54, the lap, counters, minimap and timer start at x 234,
// and centred messages (letters from x 61 to 231) stay where they are.
constexpr int left_hud_max_x = 58;
constexpr int right_hud_min_x = 232;
constexpr int screen_width = 320;
constexpr int screen_height = 220;

uint32_t race_timer_retrace = 0;
bool race_timer_seen = false;
// Whether the last display list submitted had a viewport narrower than the screen (split screen).
bool split_screen_last_frame = false;

// The split-screen views of the last display list submitted, when they were widened; none otherwise.
struct View {
    int x0, x1;         // screen pixels
    int left, right;    // the origins their edges follow
};
constexpr int max_views = 4;
View split_views[max_views];
int split_view_count = 0;
// Where func_80087110 has appended commands since split screen started: the HUD's list.
uint32_t hud_list_lo = 0;
uint32_t hud_list_hi = 0;

// The 4-player track map ("Split-Screen Map"). Where the four views meet, the game draws a 32-pixel picture of the
// track, MAP_MD, in place of the 64-pixel MAP_LG that 1 to 3 players see, and the cars' markers on it. At Large,
// func_80109468 is given MAP_LG to load (us.toml), and the game draws it there at the same size as a 3-player race's
// map, with each marker in its place on it. It draws them in copy mode, which writes the picture's transparent colour
// too, as a dark square. Each of their rectangles becomes a call to a list that saves RT64's render state, draws it as
// the game's own 1-cycle HUD pictures are drawn (blending by the picture's alpha, the map see-through as it is for 3
// players) and restores the state. Battle and Stunt draw the same map.
constexpr int map_x0 = 128, map_y0 = 78, map_x1 = 192, map_y1 = 142;   // screen pixels
constexpr int map_size = 64, marker_size = 7;                         // the map's and a marker's, pixels
constexpr int map_min_size = 20;                                        // smaller than this, a marker
constexpr uint32_t map_alpha_color = 0xFFFFFF80;
// MAP_LG is drawn about the screen's middle, so its bottom comes down onto the clock, which the game draws below the
// small map; drawn this much higher, its bottom edge is where the small map's was.
constexpr int map_raise = 16;  // pixels
constexpr int16_t copy_mode_step = 0x1000;  // s5.10: 4 texels a pixel
// How many views the last display list submitted had, outside the menus.
int views_last_frame = 0;
// Set at func_80087110's entry for its exit: the rectangle is the map or a marker on it.
bool rect_map = false;
uint32_t map_dl_start = 0;
// The lists the map's rectangles are drawn by, a frame's worth in each of model_list_frames.
constexpr int map_list_commands = 16;
constexpr int map_list_bytes = map_list_commands * 8;
constexpr int map_lists_per_frame = 16;
uint32_t map_lists = 0;
int map_list_frame = 0;
int map_lists_used = 0;

// Set at func_80087110's entry for its exit.
int rect_origin = G_EX_ORIGIN_NONE;
uint32_t rect_dl_start = 0;
bool rect_tracked = false;
bool rect_split = false;
int rect_x0, rect_y0, rect_x1, rect_y1;

// The split-screen line of rectangles drawn so far, anchored once a rectangle that doesn't carry it on is drawn or
// the list is submitted.
struct Line {
    int x0, x1, y0, y1;             // y: the first rectangle's
    uint32_t dl_start, dl_end;      // the commands its rectangles appended
};
Line line{};
bool line_open = false;

// The lines closed in the list being built, anchored when it's submitted (anchor_lines).
constexpr int max_lines = 128;
Line lines[max_lines];
int line_count = 0;

// The boxes of fills in the HUD's list (widen_split_screen), and the origin each is anchored to.
struct Box {
    int x0, x1, y0, y1;
    int origin;
    uint32_t first;     // its first fill's command; the text drawn after it is the box's
};
constexpr int max_boxes = 64;
Box boxes[max_boxes];
int box_count = 0;

uint32_t retrace_count(uint8_t* rdram) {
    return (uint32_t)MEM_W(0x27C, (gpr)(int32_t)sched_addr);
}

uint32_t read_word(uint8_t* rdram, uint32_t addr) {
    return (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
}

void write_word(uint8_t* rdram, uint32_t addr, uint32_t value) {
    MEM_W(0, (gpr)(int32_t)addr) = (int32_t)value;
}

bool hud_moves() {
    return ultramodern::renderer::get_graphics_config().hr_option != ultramodern::renderer::HUDRatioMode::Original;
}

bool single_player_race(uint8_t* rdram) {
    // 8 retraces at 60 Hz; above 60 fps the VI runs at the frame rate.
    uint32_t window = 8 * std::max<uint32_t>(60, rush2049::game_frame_rate()) / 60;
    return race_timer_seen && retrace_count(rdram) - race_timer_retrace <= window && !split_screen_last_frame;
}

// Where an origin sits on the 4:3 screen, in pixels, and the offset that makes a 10.2 coordinate relative to it.
int origin_x(int origin) {
    return origin * screen_width / G_EX_ORIGIN_RIGHT;
}

int origin_offset(int origin) {
    return -origin * screen_width * 4 / G_EX_ORIGIN_RIGHT;
}

// The origin a view edge at x follows.
int edge_origin(int x) {
    return x <= screen_width / 4 ? G_EX_ORIGIN_LEFT
         : x >= screen_width * 3 / 4 ? G_EX_ORIGIN_RIGHT
                                     : G_EX_ORIGIN_CENTER;
}

// The origin something at x in a view follows: its left edge's, its right edge's, or in the middle third the view's
// centre (halfway between them).
int view_origin(const View& v, double x) {
    double t = (x - v.x0) / (v.x1 - v.x0);
    return t < 0.3 ? v.left : t > 0.7 ? v.right : (v.left + v.right) / 2;
}

const View* view_holding(int x0, int x1) {
    for (int i = 0; i < split_view_count; i++) {
        if (x0 >= split_views[i].x0 - 2 && x1 <= split_views[i].x1 + 2) {
            return &split_views[i];
        }
    }
    return nullptr;
}

// The origin a split-screen HUD rectangle from x0 to x1 (pixels) follows. One across the whole screen stays put.
int split_rect_origin(int x0, int x1) {
    if (x0 <= 2 && x1 >= screen_width - 2) {
        return G_EX_ORIGIN_NONE;
    }
    double cx = (x0 + x1) / 2.0;
    if (const View* v = view_holding(x0, x1)) {
        return view_origin(*v, cx);
    }
    // Outside the views: beyond them on either side, or by the nearest view edge (the 4-player timer sits on the
    // line between the views).
    int lo = screen_width, hi = 0;
    int nearest = G_EX_ORIGIN_NONE;
    double nearest_distance = screen_width;
    for (int i = 0; i < split_view_count; i++) {
        const View& v = split_views[i];
        lo = std::min(lo, v.x0);
        hi = std::max(hi, v.x1);
        for (int side = 0; side < 2; side++) {
            double d = std::abs(cx - (side ? v.x1 : v.x0));
            if (d < nearest_distance) {
                nearest_distance = d;
                nearest = side ? v.right : v.left;
            }
        }
    }
    return cx >= hi ? G_EX_ORIGIN_RIGHT : cx <= lo ? G_EX_ORIGIN_LEFT : nearest;
}

// Rewrites the G_FILLRECT + pipe sync pair at p as an extended fill rectangle, anchored like the HUD, or like the
// view it fills, or edge by edge when it's outside the views (the borders between them). A part of a box (a panel and
// its border lines, box_cx its centre) anchors like the box's centre in the view holding it. Returns the origin both
// edges follow, or -1 when they follow different ones.
int anchor_fill_rect(uint8_t* rdram, uint32_t p, double box_cx = -1) {
    uint32_t w0 = read_word(rdram, p);
    uint32_t w1 = read_word(rdram, p + 4);
    int lrx = (w0 >> 12) & 0xFFF, lry = w0 & 0xFFF;
    int ulx = (w1 >> 12) & 0xFFF, uly = w1 & 0xFFF;
    int x0 = ulx / 4, x1 = lrx / 4 + 1;
    int left, right;
    const View* box_view = nullptr;
    for (int i = 0; i < split_view_count && box_cx >= 0 && !box_view; i++) {
        if (box_cx >= split_views[i].x0 && box_cx <= split_views[i].x1) {
            box_view = &split_views[i];
        }
    }
    if (box_view) {
        left = right = view_origin(*box_view, box_cx);
    } else if (const View* v = view_holding(x0, x1)) {
        if (x1 - x0 >= (v->x1 - v->x0) * 9 / 10) {
            left = v->left;
            right = v->right;
        } else {
            left = right = view_origin(*v, (x0 + x1) / 2.0);
        }
    } else {
        left = edge_origin(x0);
        right = edge_origin(x1);
    }
    write_word(rdram, p, PARAM(RT64_EXTENDED_OPCODE, 8, 24) | PARAM(G_EX_FILLRECT_V1, 24, 0));
    write_word(rdram, p + 4, PARAM(left, 12, 0) | PARAM(right, 12, 12));
    write_word(rdram, p + 8, PARAM(ulx + origin_offset(left), 16, 16) | PARAM(uly, 16, 0));
    write_word(rdram, p + 12, PARAM(lrx + origin_offset(right), 16, 16) | PARAM(lry, 16, 0));
    return left == right ? left : -1;
}

bool fill_rect_pair(uint8_t* rdram, uint32_t p) {
    return (read_word(rdram, p) >> 24) == 0xF6 && read_word(rdram, p + 8) == 0xE7000000 &&
           read_word(rdram, p + 12) == 0;
}

// Whether the fill + pipe sync pair at p covers the whole 4:3 screen.
bool full_screen_fill(uint8_t* rdram, uint32_t p) {
    uint32_t w0 = read_word(rdram, p), w1 = read_word(rdram, p + 4);
    return fill_rect_pair(rdram, p) && ((w1 >> 12) & 0xFFF) == 0 && (w1 & 0xFFF) == 0 &&
           (int)((w0 >> 12) & 0xFFF) / 4 >= screen_width - 1 && (int)(w0 & 0xFFF) / 4 >= screen_height - 1;
}

// A display list matrix (s15.16: the 16 integer halves, then the 16 fraction halves).
struct Matrix {
    double m[4][4];
};

Matrix read_matrix(uint8_t* rdram, uint32_t addr) {
    Matrix r;
    for (int i = 0; i < 16; i++) {
        int shift = (i & 1) ? 0 : 16;
        int16_t whole = (int16_t)(read_word(rdram, addr + (i / 2) * 4) >> shift);
        uint16_t frac = (uint16_t)(read_word(rdram, addr + 32 + (i / 2) * 4) >> shift);
        r.m[i / 4][i % 4] = whole + frac / 65536.0;
    }
    return r;
}

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            for (int k = 0; k < 4; k++) {
                r.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }
    return r;
}

// Each anchored model gets a list of 15 commands; lists rotate over a few frames so RT64 can still be reading the
// previous ones. Split screen uses one more list per frame, for the views and the borders between them.
constexpr int model_list_bytes = 15 * 8;
constexpr int models_per_frame = 8;  // two coins in each of four views
constexpr int model_list_frames = 4;
uint32_t model_lists = 0;
uint32_t model_list_frame = 0;
constexpr int view_list_commands = 8;
constexpr int max_border_commands = 32;
constexpr int split_list_bytes = (max_views * view_list_commands + max_border_commands + 5) * 8;
uint32_t split_lists = 0;
uint32_t split_list_frame = 0;

uint32_t alloc_lists(uint8_t* rdram, int bytes) {
    void* lists = recomp::alloc(rdram, bytes);
    return lists ? (uint32_t)((uint8_t*)lists - rdram) + 0x80000000 : 0;
}

void write_commands(uint8_t* rdram, uint32_t addr, const GfxCommand* cmds, int count) {
    for (int i = 0; i < count; i++) {
        write_word(rdram, addr + i * 8, cmds[i].values.word0);
        write_word(rdram, addr + i * 8 + 4, cmds[i].values.word1);
    }
}

// The map and its markers, by their size: the clock's digits are drawn across the bottom of the same square.
bool on_race_map(int x0, int y0, int x1, int y1) {
    int w = x1 - x0, h = y1 - y0;
    bool map = std::abs(w - map_size) <= 1 && std::abs(h - map_size) <= 1;
    bool marker = std::abs(w - marker_size) <= 1 && std::abs(h - marker_size) <= 1;
    return views_last_frame == 4 && rush2049::split_screen_map_large() && (map || marker) &&
        x0 >= map_x0 - 2 && y0 >= map_y0 - 2 && x1 <= map_x1 + 2 && y1 <= map_y1 + 2;
}

void write_call(uint8_t* rdram, uint32_t p, uint32_t list);
void write_commands(uint8_t* rdram, uint32_t addr, const GfxCommand* cmds, int count);

// Draws each copy-mode texture rectangle triple between start and end as the HUD's 1-cycle pictures are drawn: the
// map see-through, the markers on it solid.
void blend_map_rects(uint8_t* rdram, uint32_t start, uint32_t end) {
    if (map_lists == 0) {
        map_lists = alloc_lists(rdram, map_list_bytes * map_lists_per_frame * model_list_frames);
    }
    for (uint32_t p = start; map_lists != 0 && p + 24 <= end; p += 8) {
        uint32_t w0 = read_word(rdram, p);
        if ((w0 >> 24) != 0xE4 || (read_word(rdram, p + 8) >> 24) != 0xE1 || (read_word(rdram, p + 16) >> 24) != 0xF1 ||
            map_lists_used == map_lists_per_frame) {
            continue;
        }
        uint32_t w1 = read_word(rdram, p + 4);
        uint32_t steps = read_word(rdram, p + 20);
        // A marker whose car is fading in or out is drawn in 1-cycle mode already, see-through at the game's own alpha
        // (its step is 1 texel a pixel, not copy mode's 4): it is only raised with the map, where it is.
        if ((int16_t)(steps >> 16) != copy_mode_step) {
            int raised0 = std::max((int)(w1 & 0xFFF) - map_raise * 4, 0), raised1 = std::max((int)(w0 & 0xFFF) - map_raise * 4, 0);
            write_word(rdram, p, (w0 & 0xFFFFF000) | raised1);
            write_word(rdram, p + 4, (w1 & 0xFFFFF000) | raised0);
            p += 16;
            continue;
        }
        // Copy mode's lower right corner is the last pixel drawn; 1-cycle mode's is the edge after it.
        int x1 = std::min(((w0 >> 12) & 0xFFF) + 4, 0xFFFu), y1 = std::min((w0 & 0xFFF) + 4, 0xFFFu);
        int x0 = (w1 >> 12) & 0xFFF, y0 = w1 & 0xFFF;
        bool marker = x1 - x0 < map_min_size * 4;
        y0 = std::max(y0 - map_raise * 4, 0), y1 = std::max(y1 - map_raise * 4, 0);
        // Copy mode steps 4 texels a pixel (it writes four at once); 1-cycle mode, 1.
        int dsdx = (int16_t)(steps >> 16) / 4;
        int dtdy = (int16_t)(steps & 0xFFFF);

        GfxCommand cmds[map_list_commands]{};
        GfxCommand* c = cmds;
        gEXPushOtherMode(c++);
        gEXPushCombineMode(c++);
        gEXPushPrimColor(c++);
        auto raw = [&](uint32_t a, uint32_t b) { c->values.word0 = a; c->values.word1 = b; c++; };
        raw(0xE3000A01, 0x00000000);  // cycle type: 1-cycle
        raw(0xE3001201, 0x00000000);  // texture filter: point
        raw(0xE3000C00, 0x00000000);  // texture perspective off: a rectangle has no W to divide by
        // The render mode and combiner of the game's own 1-cycle HUD pictures, under the primitive color the game draws
        // the map with in a 3-player race (white, half alpha), or solid white for a marker.
        raw(0xE200001C, 0x00504240);
        raw(0xFC119623, 0xFF2FFFFF);
        raw(0xFA000000, marker ? 0xFFFFFFFF : map_alpha_color);
        raw(0xE4000000 | (x1 << 12) | y1, (w1 & 0xFF000000) | (x0 << 12) | y0);
        raw(0xE1000000, read_word(rdram, p + 12));
        raw(0xF1000000, ((uint32_t)(uint16_t)dsdx << 16) | (uint16_t)dtdy);
        gEXPopPrimColor(c++);
        gEXPopCombineMode(c++);
        gEXPopOtherMode(c++);
        raw(0xDF000000, 0);  // gSPEndDisplayList
        uint32_t list = map_lists + (map_list_frame * map_lists_per_frame + map_lists_used++) * map_list_bytes;
        write_commands(rdram, list, cmds, (int)(c - cmds));
        write_call(rdram, p, list);
        p += 16;
    }
}

// Rewrites each texture rectangle triple between start and end as an extended one anchored to origin.
void anchor_rects(uint8_t* rdram, uint32_t start, uint32_t end, int origin) {
    int x_offset = origin_offset(origin);
    for (uint32_t p = start; p + 24 <= end; p += 8) {
        uint32_t w0 = read_word(rdram, p);
        if ((w0 >> 24) != 0xE4 || (read_word(rdram, p + 8) >> 24) != 0xE1 || (read_word(rdram, p + 16) >> 24) != 0xF1) {
            continue;
        }
        uint32_t w1 = read_word(rdram, p + 4);
        int lrx = (w0 >> 12) & 0xFFF, lry = w0 & 0xFFF;
        int ulx = (w1 >> 12) & 0xFFF, uly = w1 & 0xFFF;
        uint32_t tile = (w1 >> 24) & 7;
        uint32_t st = read_word(rdram, p + 12);
        uint32_t dsdxdtdy = read_word(rdram, p + 20);
        GfxCommand cmds[3]{};
        gEXTextureRectangle(cmds, origin, origin, ulx + x_offset, uly, lrx + x_offset, lry, tile,
                            st >> 16, st & 0xFFFF, dsdxdtdy >> 16, dsdxdtdy & 0xFFFF);
        write_commands(rdram, p, cmds, 3);
        p += 16;
    }
}

// Whether a split-screen rectangle from (x0, y0) to (x1, y1), its commands starting at dl, carries the open line on:
// drawn right after it, on the same line (overlapping it by half its height), starting no further on than a letter's
// height past its end, and in the same view.
bool carries_line_on(int x0, int y0, int x1, int y1, uint32_t dl) {
    if (!line_open || dl < line.dl_end || dl - line.dl_end > 256) {
        return false;
    }
    int height = std::min(y1 - y0, line.y1 - line.y0);
    return std::min(y1, line.y1) - std::max(y0, line.y0) >= height / 2 && x0 >= line.x0 &&
           x0 <= line.x1 + (line.y1 - line.y0) &&
           view_holding(line.x0, std::max(x1, line.x1)) == view_holding(line.x0, line.x1);
}

void close_line() {
    if (line_open) {
        line_open = false;
        if (line_count < max_lines) {
            lines[line_count++] = line;
        }
    }
}

// Anchors the lines of the list being submitted: one drawn after a box, with its middle in it, as the box, others by
// where they sit.
void anchor_lines(uint8_t* rdram) {
    for (int i = 0; i < line_count; i++) {
        const Line& l = lines[i];
        double cx = (l.x0 + l.x1) / 2.0, cy = (l.y0 + l.y1) / 2.0;
        int origin = split_rect_origin(l.x0, l.x1);
        for (int j = 0; j < box_count; j++) {
            const Box& b = boxes[j];
            if (l.dl_start > b.first && cx >= b.x0 && cx <= b.x1 && cy >= b.y0 && cy <= b.y1) {
                origin = b.origin;
                break;
            }
        }
        if (origin != G_EX_ORIGIN_NONE) {
            anchor_rects(rdram, l.dl_start, l.dl_end, origin);
        }
    }
}

// Writes a call to the list at `list` at p, in the place of 3 commands.
void write_call(uint8_t* rdram, uint32_t p, uint32_t list) {
    // Addresses with the top bit set are read unmasked only in extended mode. The frame's list is in it from its start
    // (src/frame_list.cpp), and stays in it after the call, for the lists the game writes inside its own list.
    GfxCommand call[3]{};
    gEXSetRDRAMExtended(&call[0], 1);
    call[1].values.word0 = 0xDE000000;  // gSPDisplayList
    call[1].values.word1 = list;
    gEXSetRDRAMExtended(&call[2], 1);
    write_commands(rdram, p, call, 3);
}

// Replaces the matrix + call + pop at p with a call to a list at `list` that draws them under the viewport at
// `viewport` anchored to `origin`, with the scissor opened to the window's edges.
void anchor_model(uint8_t* rdram, uint32_t p, uint32_t list, uint32_t viewport, int origin) {
    GfxCommand cmds[15]{};
    GfxCommand* c = cmds;
    gEXPushViewport(c++);
    gEXPushScissor(c++);
    gEXSetViewportAlign(c, origin, origin_offset(origin), 0);
    c += 2;
    c->values.word0 = 0xDC080008;  // gSPViewport
    c->values.word1 = viewport;
    c++;
    gEXSetScissor(c, 0 /* G_SC_NON_INTERLACE */, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, 0, screen_height);
    c += 2;
    for (int i = 0; i < 3; i++, c++) {
        c->values.word0 = read_word(rdram, p + i * 8);
        c->values.word1 = read_word(rdram, p + i * 8 + 4);
    }
    gEXPopScissor(c++);
    gEXPopViewport(c++);
    gEXSetViewportAlign(c, G_EX_ORIGIN_NONE, 0, 0);
    c += 2;
    c->values.word0 = 0xDF000000;  // gSPEndDisplayList
    c->values.word1 = 0;
    write_commands(rdram, list, cmds, 15);
    write_call(rdram, p, list);
}

// What a walk over a frame's top-level display list found.
struct FrameScan {
    uint32_t hud_scissor = 0;
    uint32_t first_pipe_sync = 0;
    bool split_screen = false;
    // Each view's viewport + pipe sync + scissor.
    struct ViewTriple {
        uint32_t p, viewport;
    } views[max_views + 1];
    int view_count = 0;
    // The first and last fill + pipe sync pairs before the first view: the borders between the views.
    uint32_t first_fill = 0, last_fill = 0;
    // The lists called after the HUD scissor.
    uint32_t hud_calls[16];
    int hud_call_count = 0;
    // The HUD's 3D models (the coins): where they are, under which viewport, and where they land across it (-1 to 1).
    struct HudModel {
        uint32_t p, viewport;
        double x;
    } models[models_per_frame];
    int model_count = 0;
};

// Widens the split-screen views, their borders and their HUD. False if the views aren't a layout this knows.
bool widen_split_screen(uint8_t* rdram, const FrameScan& scan) {
    View views[max_views];
    int count = scan.view_count;
    if (count < 2 || count > max_views) {
        return false;
    }
    int lo = screen_width, hi = 0;
    bool stretched = false;
    for (int i = 0; i < count; i++) {
        uint32_t vp = scan.views[i].viewport;
        int sx = (int16_t)(read_word(rdram, vp) >> 16), sy = (int16_t)read_word(rdram, vp);
        int tx = (int16_t)(read_word(rdram, vp + 8) >> 16);
        int x0 = (tx - sx) / 4, x1 = (tx + sx) / 4;
        if (x1 - x0 < 150 || 2 * sy / 4 < 100) {
            return false;
        }
        views[i] = { x0, x1, edge_origin(x0), edge_origin(x1) };
        lo = std::min(lo, x0);
        hi = std::max(hi, x1);
        stretched |= views[i].left != views[i].right;
    }
    if (lo > 4 || hi < screen_width * 3 / 4 || !stretched) {
        return false;
    }
    std::copy(views, views + count, split_views);
    split_view_count = count;
    box_count = 0;

    if (split_lists == 0) {
        split_lists = alloc_lists(rdram, split_list_bytes * model_list_frames);
        if (split_lists == 0) {
            return true;
        }
    }
    uint32_t list = split_lists + split_list_frame * split_list_bytes;
    split_list_frame = (split_list_frame + 1) % model_list_frames;

    // Each view: its viewport with both edges anchored, then its scissor anchored the same way.
    for (int i = 0; i < count; i++, list += view_list_commands * 8) {
        const View& v = views[i];
        uint32_t p = scan.views[i].p;
        uint32_t sc0 = read_word(rdram, p + 16), sc1 = read_word(rdram, p + 20);
        GfxCommand cmds[view_list_commands]{};
        GfxCommand* c = cmds;
        gEXSetViewportAlignEdges(c, v.left, v.right, origin_offset(v.left), 0);
        c += 2;
        c->values.word0 = 0xDC080008;  // gSPViewport
        c->values.word1 = scan.views[i].viewport;
        c++;
        gEXSetViewportAlign(c, G_EX_ORIGIN_NONE, 0, 0);
        c += 2;
        gEXSetScissor(c, (sc1 >> 24) & 3, v.left, v.right,
                      (int)((sc0 >> 12) & 0xFFF) / 4 - origin_x(v.left), (int)(sc0 & 0xFFF) / 4,
                      (int)((sc1 >> 12) & 0xFFF) / 4 - origin_x(v.right), (int)(sc1 & 0xFFF) / 4);
        c += 2;
        c->values.word0 = 0xDF000000;  // gSPEndDisplayList
        c->values.word1 = 0;
        write_commands(rdram, list, cmds, view_list_commands);
        write_call(rdram, p, list);
    }

    // The borders: fills with the screen-wide scissor that's set at the start of the frame, which would clip them to
    // the 4:3 area. The commands from the first fill to the last are copied into a list with a scissor across the
    // window and their fills anchored; a call to it takes their place, and pipe syncs the rest.
    if (scan.first_fill != 0 && scan.last_fill >= scan.first_fill) {
        int n = (int)(scan.last_fill - scan.first_fill) / 8 + 2;
        bool simple = n >= 3 && n <= max_border_commands &&
                      !(scan.first_pipe_sync >= scan.first_fill && scan.first_pipe_sync <= scan.last_fill + 8);
        for (int i = 0; simple && i < n; i++) {
            switch (read_word(rdram, scan.first_fill + i * 8) >> 24) {
                case 0xF6: case 0xE7: case 0xF7: case 0xFF: case 0xFE: case 0xE3: case 0xE2: case 0xD9:
                case 0xFA: case 0xF9: case 0xF8: case 0xFC: case 0xE6: case 0xE8: case 0xE9:
                    break;
                default:
                    simple = false;
            }
        }
        if (simple) {
            // The frame's clear, when it's the first fill, stays the game's, ahead of the scissor (the header says why).
            int lead = full_screen_fill(rdram, scan.first_fill) ? 2 : 0;
            GfxCommand head[3]{};
            gEXPushScissor(&head[0]);
            gEXSetScissor(&head[1], 0 /* G_SC_NON_INTERLACE */, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, 0,
                          screen_height);
            uint32_t out = list;
            for (int i = 0; i < n; i++, out += 8) {
                if (i == lead) {
                    write_commands(rdram, out, head, 3);
                    out += 3 * 8;
                }
                write_word(rdram, out, read_word(rdram, scan.first_fill + i * 8));
                write_word(rdram, out + 4, read_word(rdram, scan.first_fill + i * 8 + 4));
                if (i >= lead && i + 1 < n && fill_rect_pair(rdram, scan.first_fill + i * 8)) {
                    write_word(rdram, out + 8, 0xE7000000);
                    write_word(rdram, out + 12, 0);
                    anchor_fill_rect(rdram, out);
                    i++;
                    out += 8;
                }
            }
            GfxCommand tail[2]{};
            gEXPopScissor(&tail[0]);
            tail[1].values.word0 = 0xDF000000;  // gSPEndDisplayList
            write_commands(rdram, out, tail, 2);
            write_call(rdram, scan.first_fill, list);
            for (int i = 3; i < n; i++) {
                write_word(rdram, scan.first_fill + i * 8, 0xE7000000);
                write_word(rdram, scan.first_fill + i * 8 + 4, 0);
            }
        }
    }

    // The HUD's fills (the 4-player score boxes, the pause menu's panels and their border lines), in the lists it calls,
    // as far as func_80087110 has written. Fills that touch make a box, anchored as one; a fill across a view or the
    // screen (the pause menu darkens the whole screen) stretches with it, and is part of no box.
    struct Fill {
        uint32_t p;
        int x0, x1, y0, y1;
        int box;
    };
    constexpr int max_fills = 64;
    Fill fills[max_fills];
    int fill_count = 0;
    for (int i = 0; i < scan.hud_call_count && hud_list_lo != 0; i++) {
        uint32_t p = scan.hud_calls[i];
        for (int n = 0; n < 8000; n++, p += 8) {
            uint32_t w0 = read_word(rdram, p);
            if ((w0 >> 24) == 0xDF) {
                break;
            }
            if ((w0 >> 24) == 0xDE && (w0 & 0x00010000)) {
                p = (read_word(rdram, p + 4) | 0x80000000) - 8;
                continue;
            }
            if (p >= hud_list_lo && p + 16 <= hud_list_hi && fill_rect_pair(rdram, p) && fill_count < max_fills &&
                std::none_of(fills, fills + fill_count, [&](const Fill& f) { return f.p == p; })) {
                uint32_t w1 = read_word(rdram, p + 4);
                fills[fill_count] = { p, (int)((w1 >> 12) & 0xFFF) / 4, (int)((w0 >> 12) & 0xFFF) / 4 + 1,
                                      (int)(w1 & 0xFFF) / 4, (int)(w0 & 0xFFF) / 4 + 1, fill_count };
                fill_count++;
                p += 8;
            }
        }
    }
    auto box_of = [&](int i) {
        while (fills[i].box != i) {
            i = fills[i].box;
        }
        return i;
    };
    auto spans = [&](const Fill& f) {
        const View* v = view_holding(f.x0, f.x1);
        return f.x1 - f.x0 >= (v ? v->x1 - v->x0 : screen_width) * 9 / 10;
    };
    for (int i = 0; i < fill_count; i++) {
        for (int j = 0; j < i; j++) {
            const Fill &a = fills[i], &b = fills[j];
            if (!spans(a) && !spans(b) && a.x0 <= b.x1 + 1 && b.x0 <= a.x1 + 1 && a.y0 <= b.y1 + 1 &&
                b.y0 <= a.y1 + 1) {
                fills[box_of(i)].box = box_of(j);
            }
        }
    }
    for (int i = 0; i < fill_count; i++) {
        int box = box_of(i), parts = 0;
        Box b{ screen_width, 0, screen_height, 0, -1, fills[i].p };
        for (int j = 0; j < fill_count; j++) {
            if (box_of(j) == box) {
                parts++;
                b.x0 = std::min(b.x0, fills[j].x0);
                b.x1 = std::max(b.x1, fills[j].x1);
                b.y0 = std::min(b.y0, fills[j].y0);
                b.y1 = std::max(b.y1, fills[j].y1);
                b.first = std::min(b.first, fills[j].p);
            }
        }
        b.origin = anchor_fill_rect(rdram, fills[i].p, parts > 1 ? (b.x0 + b.x1) / 2.0 : -1);
        // A box once, at its first fill (box_of's root); a fill across a view or the screen holds no text of its own.
        if (box == i && !spans(fills[i]) && b.origin >= 0 && box_count < max_boxes) {
            boxes[box_count++] = b;
        }
    }

    // The coins: anchored like the HUD beside them, in their own view.
    if (scan.model_count > 0 && model_lists == 0) {
        model_lists = alloc_lists(rdram, model_list_bytes * models_per_frame * model_list_frames);
    }
    if (model_lists != 0) {
        uint32_t frame_lists = model_lists + model_list_frame * model_list_bytes * models_per_frame;
        model_list_frame = (model_list_frame + 1) % model_list_frames;
        for (int i = 0; i < scan.model_count; i++) {
            const FrameScan::HudModel& m = scan.models[i];
            for (int j = 0; j < count; j++) {
                if (scan.views[j].viewport == m.viewport) {
                    int sx = (int16_t)(read_word(rdram, m.viewport) >> 16);
                    int tx = (int16_t)(read_word(rdram, m.viewport + 8) >> 16);
                    double x = (tx + sx * m.x) / 4;
                    anchor_model(rdram, m.p, frame_lists + i * model_list_bytes, m.viewport, view_origin(views[j], x));
                    break;
                }
            }
        }
    }
    return true;
}

// Menus: the background picture (320x220) is texture rectangles, rows 256 pixels wide from the left edge and a column
// 64 wide at the right edge. Each one at an edge is replaced by a call to a list that widens the scissor, draws it,
// and draws its mirror image across the screen edge into the side bar.
constexpr int menu_list_commands = 15;
constexpr int menu_list_bytes = menu_list_commands * 8;
constexpr int menu_lists_per_frame = 96;
uint32_t menu_lists = 0;
int menu_list_frame = 0;
int menu_lists_used = 0;
bool rect_menu = false;
// Whether the display list being built has drawn the menu background. The car select of Ghost Race (and of the
// multiplayer modes) draws each panel's car under a viewport of its own, laid out like split screen, so a list with
// the background in it is a menu and never widened as split screen.
bool menu_background_drawn = false;

// The background's strips (draw_tex_rect's corners are inclusive). The title screen's picture is laid out the same way
// but drawn in copy mode (4 texels a step in s); it's a picture rather than a pattern that carries on, so only strips
// drawn one texel to a pixel are widened.
bool is_menu_background(int x0, int y0, int x1, int y1) {
    return ((x0 == 0 && x1 == 255) || (x0 == 256 && x1 == screen_width - 1)) && y1 - y0 < 32;
}

// Mirrored copies reach this far into each side bar, in pixels: 21:9 or so.
constexpr int menu_bar_width = 128;

// Replaces the texture rectangle triple at p, a strip of the menu background at the left or right edge of the 4:3
// screen, with a call to a list at `list` that draws it and fills the side bar beside it.
void widen_menu_rect(uint8_t* rdram, uint32_t p, uint32_t list) {
    uint32_t w0 = read_word(rdram, p), w1 = read_word(rdram, p + 4);
    int lrx = (w0 >> 12) & 0xFFF, lry = w0 & 0xFFF;  // 10.2
    int ulx = (w1 >> 12) & 0xFFF, uly = w1 & 0xFFF;
    int tile = (w1 >> 24) & 7;
    uint32_t st = read_word(rdram, p + 12), d = read_word(rdram, p + 20);
    int s = (int16_t)(st >> 16), t = (int16_t)st;      // s10.5
    int dsdx = (int16_t)(d >> 16), dtdy = (int16_t)d;  // s5.10 a pixel, so dsdx / 128 a quarter pixel in s
    auto s_at = [&](int x) { return s + (x - ulx) * dsdx / 128; };

    GfxCommand cmds[menu_list_commands]{};
    GfxCommand* c = cmds;
    gEXPushScissor(c++);
    gEXSetScissor(c, 0 /* G_SC_NON_INTERLACE */, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, 0, screen_height);
    c += 2;
    auto rect = [&](int x0, int x1, int s0, int ds) {
        gEXTextureRectangle(c, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, x0, uly, x1, lry, tile, s0, t, ds, dtdy);
        c += 3;
    };
    // The strip, its mirror image across the screen edge (starting from the texel at the edge), and, when the strip is
    // narrower than the bar (the right-hand column), the strip again beyond that.
    rect(ulx, lrx, s, dsdx);
    int m = std::min(lrx - ulx, menu_bar_width * 4);
    int pixel = dsdx / 32;
    if (ulx == 0) {
        rect(-m, 0, s_at(m) - pixel, -dsdx);
        if (m < menu_bar_width * 4) {
            rect(-2 * m, -m, s, dsdx);
        }
    } else {
        rect(lrx, lrx + m, s_at(lrx) - pixel, -dsdx);
        if (m < menu_bar_width * 4) {
            rect(lrx + m, lrx + 2 * m, s_at(lrx - m), dsdx);
        }
    }
    gEXPopScissor(c++);
    c->values.word0 = 0xDF000000;  // gSPEndDisplayList
    c->values.word1 = 0;
    c++;
    write_commands(rdram, list, cmds, (int)(c - cmds));
    write_call(rdram, p, list);
}

} // namespace

bool rush2049::split_screen() {
    return split_screen_last_frame;
}

extern "C" void rush2049_hud_race_mark(uint8_t* rdram) {
    race_timer_retrace = retrace_count(rdram);
    race_timer_seen = true;
}

extern "C" void rush2049_hud_rect_begin(uint8_t* rdram, recomp_context* ctx) {
    rect_origin = G_EX_ORIGIN_NONE;
    rect_tracked = false;
    rect_split = false;
    rect_menu = false;
    int x0 = (int)ctx->r4;
    int x1 = (int)ctx->r6;
    int y0 = (int)ctx->r5;
    int y1 = (int)ctx->r7;
    rect_map = on_race_map(x0, y0, x1, y1);
    if (rect_map) {
        map_dl_start = read_word(rdram, dl_ptr_addr);
    }
    if (line_open && !(split_view_count > 0 && carries_line_on(x0, y0, x1, y1, read_word(rdram, dl_ptr_addr)))) {
        close_line();
    }
    if (is_menu_background(x0, y0, x1, y1) && !single_player_race(rdram)) {
        menu_background_drawn = true;
    }
    if (rush2049::menu_expand() && split_view_count == 0 && menu_background_drawn &&
        is_menu_background(x0, y0, x1, y1)) {
        rect_menu = true;
        rect_dl_start = read_word(rdram, dl_ptr_addr);
        return;
    }
    if (!hud_moves()) {
        return;
    }
    if (split_view_count > 0) {
        rect_split = true;
        rect_tracked = true;
        rect_x0 = x0, rect_y0 = y0, rect_x1 = x1, rect_y1 = y1;
    } else if (!single_player_race(rdram)) {
        return;
    } else if (x1 <= left_hud_max_x) {
        rect_origin = G_EX_ORIGIN_LEFT;
    } else if (x0 >= right_hud_min_x) {
        rect_origin = G_EX_ORIGIN_RIGHT;
    } else {
        return;
    }
    rect_dl_start = read_word(rdram, dl_ptr_addr);
}

extern "C" gpr rush2049_split_map_name(gpr name) {
    constexpr uint32_t map_lg_name = 0x80120D04;  // "MAP_LG"
    return rush2049::split_screen_map_large() ? (gpr)(int32_t)map_lg_name : name;
}

extern "C" void rush2049_hud_rect_end(uint8_t* rdram) {
    if (rect_map) {
        rect_map = false;
        blend_map_rects(rdram, map_dl_start, read_word(rdram, dl_ptr_addr));
    }
    if (rect_menu) {
        rect_menu = false;
        if (menu_lists == 0) {
            menu_lists = alloc_lists(rdram, menu_list_bytes * menu_lists_per_frame * model_list_frames);
        }
        uint32_t end = read_word(rdram, dl_ptr_addr);
        for (uint32_t p = rect_dl_start; menu_lists != 0 && p + 24 <= end; p += 8) {
            if ((read_word(rdram, p) >> 24) != 0xE4 || (read_word(rdram, p + 8) >> 24) != 0xE1 ||
                (read_word(rdram, p + 16) >> 24) != 0xF1 || (read_word(rdram, p + 20) >> 16) != 0x400 ||
                menu_lists_used == menu_lists_per_frame) {
                continue;
            }
            uint32_t list = menu_lists + (menu_list_frame * menu_lists_per_frame + menu_lists_used++) * menu_list_bytes;
            widen_menu_rect(rdram, p, list);
            p += 16;
        }
        return;
    }
    if (rect_origin == G_EX_ORIGIN_NONE && !rect_tracked) {
        return;
    }
    uint32_t end = read_word(rdram, dl_ptr_addr);
    if (rect_tracked) {
        hud_list_lo = hud_list_lo == 0 ? rect_dl_start : std::min(hud_list_lo, rect_dl_start);
        hud_list_hi = std::max(hud_list_hi, end);
        rect_tracked = false;
    }
    if (rect_split) {
        rect_split = false;
        if (line_open) {
            line.x0 = std::min(line.x0, rect_x0);
            line.x1 = std::max(line.x1, rect_x1);
            line.dl_end = end;
        } else {
            line = { rect_x0, rect_x1, rect_y0, rect_y1, rect_dl_start, end };
            line_open = true;
        }
        return;
    }
    if (rect_origin == G_EX_ORIGIN_NONE) {
        return;
    }
    anchor_rects(rdram, rect_dl_start, end, rect_origin);
    rect_origin = G_EX_ORIGIN_NONE;
}

extern "C" void rush2049_enable_extended_gbi(uint8_t* rdram, uint32_t task_addr) {
    gpr task = (gpr)(int32_t)task_addr;
    if (MEM_W(0x00, task) != 1) {  // M_GFXTASK
        return;
    }
    close_line();
    map_list_frame = (map_list_frame + 1) % model_list_frames;
    map_lists_used = 0;
    // The list is walked at any HUD Placement and Menu Aspect Ratio, to tell split screen (rewind needs to know), but
    // only changed when the HUD moves or the menus widen.
    bool menus = rush2049::menu_expand();
    if (hud_moves() || menus) {
        menu_list_frame = (menu_list_frame + 1) % model_list_frames;
        menu_lists_used = 0;
    }
    bool menu = menu_background_drawn;
    menu_background_drawn = false;
    uint32_t task_dl = (uint32_t)MEM_W(0x30, task) | 0x80000000;
    uint32_t dl = rush2049::frame_list_game_start(task_dl);
    bool wanted = hud_moves() || menus;
    // The frame's list starts with gEXEnable (src/frame_list.cpp); a list in the game's slot gets it in place of
    // its first command.
    bool extended = wanted && (dl != task_dl || (read_word(rdram, dl) == 0xDB060000 && read_word(rdram, dl + 4) == 0));  // gsSPSegment(0, 0)
    if (extended && dl == task_dl) {
        GfxCommand cmd{};
        gEXEnable(&cmd);
        write_word(rdram, dl, cmd.values.word0);
        write_word(rdram, dl + 4, cmd.values.word1);
    }
    bool hud = extended && hud_moves();

    bool in_race = single_player_race(rdram);

    // The HUD is drawn after the frame's last full-screen scissor, (0,0)-(320,220), which would clip the moved
    // rectangles to the 4:3 area. That scissor follows a pipe sync (nothing to RT64), so the pair makes room for an
    // extended scissor from the window's left edge to its right edge. The list is walked at its top level only.
    FrameScan scan;
    // The world's projection and view so far, to find which edge a HUD model sits by.
    Matrix proj{}, view{};
    uint32_t viewport = 0;
    uint32_t p = dl + 8;
    for (int n = 0; n < 20000; n++, p += 8) {
        uint32_t w0 = read_word(rdram, p);
        uint32_t w1 = read_word(rdram, p + 4);
        if ((w0 >> 24) == 0xDF) {
            break;
        }
        if ((w0 >> 24) == 0xDE && (w0 & 0x00010000)) {  // branch: the list continues there
            p = (w1 | 0x80000000) - 8;
            continue;
        }
        if ((w0 >> 24) == 0xDE && scan.hud_scissor != 0 && scan.hud_call_count < 16) {
            scan.hud_calls[scan.hud_call_count++] = w1 | 0x80000000;
        }
        if (w0 == 0xDC080008 && (w1 >> 24) == 0x80) {  // gSPViewport with a direct address
            int16_t half_width = (int16_t)(read_word(rdram, w1) >> 16) / 4;
            if (half_width < screen_width / 2) {
                scan.split_screen = true;
            }
            viewport = w1;
            if (read_word(rdram, p + 8) == 0xE7000000 && read_word(rdram, p + 12) == 0 &&
                (read_word(rdram, p + 16) >> 24) == 0xED && scan.view_count <= max_views) {
                scan.views[scan.view_count++] = { p, w1 };
            }
        }
        if (scan.view_count == 0 && fill_rect_pair(rdram, p)) {
            if (scan.first_fill == 0) {
                scan.first_fill = p;
            }
            scan.last_fill = p;
        }
        if ((w0 & 0xFFFFFF00) == 0xDA380000 && (w1 >> 24) == 0x80) {  // gSPMatrix with a direct address
            uint32_t params = (w0 & 0xFF) ^ 1;  // G_MTX_PUSH is inverted in F3DEX2
            if (params & 4) {  // G_MTX_PROJECTION
                proj = (params & 2) ? read_matrix(rdram, w1) : multiply(read_matrix(rdram, w1), proj);
            } else if (params & 2) {  // G_MTX_LOAD
                view = read_matrix(rdram, w1);
            } else if (params == 1 && scan.model_count < models_per_frame && viewport != 0 &&
                       read_word(rdram, p + 8) == 0xDE000000 && read_word(rdram, p + 16) == 0xD8380002 &&
                       read_word(rdram, p + 20) == 0x40) {  // pushed, then a call and a pop
                Matrix model = read_matrix(rdram, w1);
                if (model.m[3][3] == 10.0) {
                    Matrix clip = multiply(multiply(model, view), proj);
                    double w = clip.m[3][3];
                    if (w > 0) {
                        scan.models[scan.model_count++] = { p, viewport, clip.m[3][0] / w };
                    }
                }
            }
        }
        if (w0 == 0xE7000000 && w1 == 0 && read_word(rdram, p + 8) == 0xED000000 &&
            read_word(rdram, p + 12) == 0x00500370) {
            scan.hud_scissor = p;
            scan.hud_call_count = 0;
        }
        if (w0 == 0xE7000000 && w1 == 0 && scan.first_pipe_sync == 0 && (read_word(rdram, p + 8) >> 24) != 0xED) {
            scan.first_pipe_sync = p;
        }
    }
    // RT64 works out the game's frame rate from how often the framebuffer changes, counting retraces at 60 Hz. Above
    // 60 fps the VI is faster than that, so the list says the rate instead, in the place of a pipe sync. Otherwise
    // RT64 would take it for 60 fps and, when its Framerate is above 60, interpolate frames it doesn't need.
    uint32_t fps = rush2049::game_frame_rate();
    if (hud && fps > 60 && scan.first_pipe_sync != 0) {
        GfxCommand rate{};
        gEXSetRefreshRate(&rate, fps);
        write_word(rdram, scan.first_pipe_sync, rate.values.word0);
        write_word(rdram, scan.first_pipe_sync + 4, rate.values.word1);
    }
    split_screen_last_frame = scan.split_screen;
    views_last_frame = scan.split_screen && !menu ? scan.view_count : 0;
    bool widened = hud && scan.split_screen && !menu && rush2049::split_screen_expand() && widen_split_screen(rdram, scan);
    rush2049::set_full_width(widened, extended && menu && menus);
    if (widened) {
        anchor_lines(rdram);
    }
    line_count = 0;
    if (!hud) {
        return;
    }
    if (!widened) {
        split_view_count = 0;
        hud_list_lo = hud_list_hi = 0;
        if (scan.split_screen || !in_race) {
            return;
        }
        if (scan.model_count > 0 && model_lists == 0) {
            model_lists = alloc_lists(rdram, model_list_bytes * models_per_frame * model_list_frames);
        }
        if (scan.model_count > 0 && model_lists != 0) {
            uint32_t frame_lists = model_lists + model_list_frame * model_list_bytes * models_per_frame;
            model_list_frame = (model_list_frame + 1) % model_list_frames;
            for (int i = 0; i < scan.model_count; i++) {
                const FrameScan::HudModel& m = scan.models[i];
                double x = screen_width / 2 * (1 + m.x);
                int origin = x >= right_hud_min_x ? G_EX_ORIGIN_RIGHT
                           : x <= left_hud_max_x  ? G_EX_ORIGIN_LEFT
                                                  : G_EX_ORIGIN_NONE;
                if (origin != G_EX_ORIGIN_NONE) {
                    anchor_model(rdram, m.p, frame_lists + i * model_list_bytes, m.viewport, origin);
                }
            }
        }
    }
    if (scan.hud_scissor == 0) {
        return;
    }
    GfxCommand cmds[2]{};
    gEXSetScissor(cmds, 0 /* G_SC_NON_INTERLACE */, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT, 0, 0, 0, screen_height);
    write_commands(rdram, scan.hud_scissor, cmds, 2);
}
