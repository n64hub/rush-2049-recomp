// Hi-res font. Every font in the game (menus, race HUD, messages) is a 4-bit intensity atlas, 96 texels wide and
// about 8-13 texels per glyph, drawn as texture rectangles, so at window resolution the text is blocky. RT64 can
// replace a texture with a bigger one from a texture pack, keyed by a hash of what the texture loads into TMEM.
// The fonts come from the user's ROM, so no pack can ship in this repo. Instead the game makes its own: before
// each graphics task starts, the display list is walked with the same TMEM loads RT64 does, and every intensity
// texture a texture rectangle draws is hashed the way RT64 hashes it. A hash not seen before is upscaled 4x and
// written to hires_font/ in the app folder, which is registered with RT64 as a built-in pack below any texture
// packs the user enables. Only the first frame that draws a new font shows the original; after that the pack
// already has it, across restarts too.
// Each new glyph makes its atlas's replacement again and reloads the pack, 30-36 times when a fresh profile walks the
// menus into a race. On the web page that destroyed an old replacement while a list that had bound it waited to be
// executed ("Destroyed texture [384x136 RGBA8Unorm] used in a submit", once or twice a walk in Chrome 155), which
// plume be0524c fixed, and RT64's texture upload thread installed replacements from before a reload into a list
// recorded after they were destroyed (about one walk in three in Firefox 158, whose longer GPU wait widens the window;
// a test build with a 40 ms sleep in that window logged 115 such textures in Chrome), which rt64 918deb3 fixed. With
// both, 5 walks in Chrome and 3 in Firefox logged none.
//
// The atlases are anti-aliased (rasterised from a vector font), so the upscale is a smooth bicubic followed by a
// moderate contrast curve around half the atlas's brightest value: edges come out clean without closing the
// one-texel gaps inside glyphs, which a hard threshold or a pixel-art scaler does.
//
// An intensity texel is its own colour and alpha, and the game draws text as colour TEXEL0 * PRIM, alpha TEXEL0, so
// a half-transparent texel is also half dark: an edge pixel ends up a quarter of the text colour plus half the
// background, darker than both. Point-sampled at 320x240 that is a dim texel or two; along a smooth hi-res edge it is
// a dark outline around every letter, and the quarter texel of a glyph's soft edge that reaches into a neighbouring
// glyph's rectangle (the atlases pack glyphs edge to edge) shows as a dark line along that rectangle. So the pack's
// colour is the atlas's brightest value wherever alpha isn't 0, and alpha alone carries the edge. The letters'
// insides are unchanged, and a texel that is fully transparent stays black.
//
// The atlases pack glyphs edge to edge, and the game draws each glyph with a rectangle of exactly its own texels. A
// smooth upscale of the whole atlas reads across those edges: where one glyph's soft edge column meets the next
// glyph's stem, the stem lands in the outer quarter of that column at full brightness, and the glyph's rectangle ends
// there, so it shows as a thin line beside the letter (TRANSMISSION, AUTOMATIC, HANDLING in car select). So each glyph
// is upscaled on its own, reading only its own texels. Past its edge it reads as empty, so a soft edge column ends
// crisply inside its texel rather than as a faint band; a solid edge texel is repeated instead, so pieces drawn side
// by side (a meter drawn a column at a time) stay joined. Which texels are a glyph the game says itself: the walk keeps
// every texel box a texture rectangle draws from an atlas, beside its texture in the pack (<hash>.glyphs), and makes
// the texture again when a glyph turns up that it hasn't drawn before. A texel no rectangle has drawn yet is upscaled
// with the whole atlas around it. The upscale also puts a faint strip into an empty texel next to a full one, so a
// texel that is 0 in the atlas stays fully transparent in the pack: a vector glyph covers none of it.
//
// Hi-res title screen. The title screen's 320x220 picture is 35 copy-mode strips of an 8-bit colour-indexed texture.
// An upscaled picture of it (4x, by Real-ESRGAN) is cut into those strips the same way: each strip the walk finds is
// hashed with its palette, and its replacement is read out of the picture where the strip lands on screen. A strip
// whose texels don't match the picture shrunk back down (another screen drawn the same way) is left alone.
// The picture is made from the game's art, so the repo can't hold it. It holds assets/title.patch, which describes
// the picture in terms of the title as the game draws it and is noise without it (tools/title_patch.py says
// how). On the first task that draws the whole title, the strips make the original, which unlocks and unpacks the
// patch, and the strips are cut into hires_title/pack/ in the app folder: once per launch, about 100 ms natively and
// 150-350 on a page, on a thread of its own. The walk runs in the scheduler's thread, and while it is busy no other
// game thread runs, the audio thread included, so the cut there left the sound that long without a buffer (more
// than the 40 ms queued); the title shows the original for those frames instead. The Midway
// screen is drawn the same way but never covers the whole picture. Hi-res Title Screen (Graphics) turns it off, and
// the strips are drawn bottom-up, which needs RT64_FLIPPED_RECT_HIGH_RES (CMakeLists.txt). To make a patch for another picture, save it as hires_title/title.png in the app folder: it is used instead,
// and each run writes the title as the game draws it to hires_title/original.ppm for the tool.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "rush2049_game.h"
#include "rush2049_hooks.h"
#include "recompui/renderer.h"
#include "util/file.h"

#include "xxHash/xxh3.h"
#include "shared/rt64_f3d_defines.h"
#include "common/rt64_load_types.h"
#include "common/rt64_tmem_hasher.h"
#include "stb/stb_image.h"
#include "zstd/lib/zstd.h"

#define F3DEX_GBI_2
#include "../lib/rt64/include/rt64_extended_gbi.h"

namespace {

// Bump when the upscale changes, so textures made by an older version are thrown away.
const char* const pack_version = "v4";
constexpr int scale = 4;
constexpr float contrast = 2.5f;
// Glyphs kept for one texture, past which a new one no longer makes it again: text scrolled through a texture would
// otherwise remake it every frame. A 96-texel atlas of 5-texel glyphs has a couple of hundred.
constexpr size_t max_glyphs = 512;
// An edge texel at least this much of the brightest is solid: repeated past its glyph's box rather than ended.
constexpr float solid = 0.75f;

constexpr uint32_t tmem_bytes = 4096;
constexpr uint32_t rdram_bytes = (uint32_t)recomp::mem_size;
constexpr uint32_t fmt_rgba = 0, fmt_ci = 2, fmt_i = 4;
constexpr uint32_t siz_4b = 0, siz_8b = 1, siz_32b = 3;

// On by default: the option's change callback doesn't run on a first launch, before there is a saved config.
std::atomic<bool> enabled{ true };
std::filesystem::path pack_dir;
// Every hash already handled: in the pack, or drawn but not wanted (empty).
std::unordered_set<uint64_t> seen;
std::vector<uint64_t> in_pack;
// The texel boxes texture rectangles have drawn from each texture, [x0, x1) by [y0, y1): the glyphs of an atlas.
struct Glyph {
    uint16_t x0, y0, x1, y1;
    bool operator==(const Glyph&) const = default;
};
std::unordered_map<uint64_t, std::vector<Glyph>> atlas_glyphs;

// Where a texture rectangle lands, in pixels, and the texel at its upper left corner and texels a pixel.
struct RectTexels {
    float ulx, uly, lrx, lry, s, t, dsdx, dtdy;
};

// The upscaled title, scaled title_scale times from 320x220, and the pack cut from it.
constexpr int title_width = 320, title_height = 220;
// Mean difference per channel, out of 255, between a strip's texels and the picture shrunk back down, above which
// the strip isn't the title.
constexpr float title_match = 16.0f;
std::filesystem::path title_dir;
std::vector<uint8_t> title_rgba;
int title_scale = 0;
// On by default, as the font.
std::atomic<bool> title_enabled{ true };
// assets/title.patch, while title_rgba is still to be unpacked from it: a 28-byte header, then the payload.
constexpr size_t title_patch_header = 28;
std::vector<uint8_t> title_patch;
uint64_t title_check = 0;
// With title.png: the title as the game draws it, RGB, written to original.ppm for title_patch.py.
std::vector<uint8_t> title_original;

// A strip of the title as the walk found it: its texels as the screen pixels they fill, and how texel (u, v), in
// texels from the tile's origin, lands on screen.
struct TitleTexel {
    int x, y;
    uint8_t rgb[3];
};
struct TitleStrip {
    uint64_t hash = 0;
    float ulx = 0, uly = 0, lrx = 0, lry = 0, s0 = 0, t0 = 0, dt = 1;
    int width = 0, height = 0;
    std::vector<TitleTexel> texels;
    float screen_x(float u) const { return ulx + 0.5f + (u - s0 - 0.5f); }
    float screen_y(float v) const { return uly + 0.5f + (v - t0 - 0.5f) / dt; }
};
bool cut_title_strip(const TitleStrip& strip);

// A channel of an RGBA 5551 colour out of 255.
uint8_t channel(uint32_t c, int k) {
    return (uint8_t)(((c >> (11 - 5 * k)) & 31) * 255 / 31);
}
std::unordered_set<uint64_t> title_seen;
std::vector<uint64_t> title_in_pack;
// While the patch is unpacked and cut on its own thread, which then owns the title's state: the walk leaves it alone.
std::atomic<bool> title_cutting{ false };

struct Rdp {
    uint8_t* rdram;
    uint32_t segments[16]{};
    struct Image {
        uint32_t address = 0, fmt = 0, siz = 0, width = 0;
    } timg;
    RT64::LoadTile tiles[8]{};
    uint8_t tmem[tmem_bytes]{};
    // Which TMEM bytes hold a known texture: written by a load block. Other loads (tile, TLUT) aren't emulated,
    // so they mark what they may have overwritten as unknown, and rectangles drawing from there are skipped.
    bool tmem_known[tmem_bytes]{};
    // The loads since TMEM was last read, copied in only when a rectangle reads it (flush). Most of a frame's loads are
    // its 3D textures, which no rectangle here reads and the next load overwrites, so a load that a later one covers
    // is dropped uncopied.
    struct Load {
        bool tlut;
        Image image;
        RT64::LoadTile tile;
        uint32_t uls, ult, lrs, lrt;
        uint32_t lo, hi;  // the TMEM bytes it writes, [lo, hi), or lo == hi where they aren't one run
    };
    std::vector<Load> loads;
    std::vector<uint64_t> found;
    // Textures to make once the walk is done, new or with a glyph not drawn before, and their texels.
    struct Pending {
        uint32_t width, height;
        std::vector<float> texels;
    };
    std::unordered_map<uint64_t, Pending> pending;
    // gEXSetRDRAMExtended: the widescreen HUD calls lists of its own above the game's 8 MB with this on.
    bool extended_rdram = false;
    // The high half of the other modes, for the TLUT type.
    uint32_t othermode_h = 0;
    bool found_title = false;
    std::vector<TitleStrip> title_pending;

    // RT64's RSP::fromSegmented and maskPhysicalAddress.
    uint32_t phys(uint32_t addr) const {
        if (extended_rdram && (addr & 0x80000000)) {
            return addr - 0x80000000;
        }
        return (segments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x007FFFFF;
    }
    uint32_t rd32(uint32_t p) const { return *reinterpret_cast<uint32_t*>(rdram + (p & (rdram_bytes - 4))); }
    uint8_t rd8(uint32_t p) const { return rdram[(p & (rdram_bytes - 1)) ^ 3]; }
    static uint32_t byteswap32(uint32_t v) {
        return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
    }

    // RT64's RDP::loadBlockOperation / loadToTMEMCommon for everything but 32-bit RGBA.
    void load_block(uint32_t t, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
        RT64::LoadTile& tile = tiles[t];
        tile.uls = uls;
        tile.ult = ult;
        tile.lrs = lrs;
        tile.lrt = dxt;
        if (uls > lrs || lrs >= 0x800) {
            return;
        }
        if (tile.siz == siz_32b && tile.fmt == fmt_rgba) {
            forget_tmem();
            return;
        }
        uint32_t words = std::min(((lrs - uls) >> (4 - tile.siz)) + 1, tmem_bytes / 8);
        uint32_t lo = (tile.tmem << 3) & (tmem_bytes - 1), hi = lo + words * 8;
        if ((dxt != 0 && tile.line != 0) || hi > tmem_bytes) {  // rows apart, or wrapping around
            lo = hi = 0;
        }
        add_load({ false, timg, tile, uls, ult, lrs, dxt, lo, hi });
    }

    void add_load(const Load& load) {
        if (load.lo < load.hi) {
            std::erase_if(loads, [&](const Load& l) { return l.lo < l.hi && load.lo <= l.lo && l.hi <= load.hi; });
        }
        loads.push_back(load);
    }

    // A load tile, or a 32-bit block load, which aren't emulated: nothing in TMEM is known after one.
    void forget_tmem() {
        loads.clear();
        std::fill(std::begin(tmem_known), std::end(tmem_known), false);
    }

    // Copies the loads since the last flush into TMEM, before a rectangle reads it.
    void flush() {
        for (const Load& load : loads) {
            if (load.tlut) {
                copy_tlut(load);
            }
            else {
                copy_block(load);
            }
        }
        loads.clear();
    }

    void copy_block(const Load& load) {
        const Image& timg = load.image;
        const RT64::LoadTile& tile = load.tile;
        uint32_t uls = load.uls, ult = load.ult, lrs = load.lrs, dxt = load.lrt;
        uint32_t bytes_per_row = timg.width << timg.siz >> 1;
        uint32_t src = timg.address + (uls << timg.siz >> 1) + bytes_per_row * ult;
        uint32_t words = ((lrs - uls) >> (4 - tile.siz)) + 1;
        uint32_t dst = (tile.tmem << 3) & (tmem_bytes - 1);
        uint32_t stride = tile.line << 3;
        uint32_t xor_mask = 0, dxt_counter = 0;
        // A source on a word boundary (nearly all of them) is copied a word at a time. dst is a multiple of 8, so xor_mask
        // (0 or 4) swaps the two words.
        bool aligned = (src & 3) == 0;
        for (uint32_t w = 0; w < words && w < tmem_bytes / 8; w++) {
            if (aligned) {
                uint32_t hi = byteswap32(rd32(src)), lo = byteswap32(rd32(src + 4));
                memcpy(&tmem[dst ^ xor_mask], &hi, 4);
                memcpy(&tmem[dst ^ xor_mask ^ 4], &lo, 4);
                memset(&tmem_known[dst], 1, 8);
            }
            else {
                for (uint32_t i = 0; i < 8; i++) {
                    uint32_t a = ((dst + i) ^ xor_mask) & (tmem_bytes - 1);
                    tmem[a] = rd8(src + i);
                    tmem_known[a] = true;
                }
            }
            dxt_counter += dxt;
            while (dxt_counter >= 0x800) {
                dst = (dst + stride) & (tmem_bytes - 1);
                dxt_counter -= 0x800;
                xor_mask ^= 4;
            }
            src += 8;
            dst = (dst + 8) & (tmem_bytes - 1);
        }
    }

    // RT64's RDP::loadTLUTOperation: each 16-bit entry fills a TMEM word, four times over.
    void load_tlut(uint32_t t, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
        RT64::LoadTile& tile = tiles[t];
        tile.uls = uls;
        tile.ult = ult;
        tile.lrs = lrs;
        tile.lrt = lrt;
        uint32_t rows = 1 + (lrt >> 2) - (ult >> 2), words = std::min((lrs >> 2) - (uls >> 2) + 1, tmem_bytes / 8);
        uint32_t lo = (tile.tmem << 3) & (tmem_bytes - 1), hi = lo + words * 8;
        if (rows != 1 || hi > tmem_bytes) {
            lo = hi = 0;
        }
        add_load({ true, timg, tile, uls, ult, lrs, lrt, lo, hi });
    }

    void copy_tlut(const Load& load) {
        const Image& timg = load.image;
        const RT64::LoadTile& tile = load.tile;
        uint32_t uls = load.uls, ult = load.ult, lrs = load.lrs, lrt = load.lrt;
        uint32_t bytes_per_row = timg.width << timg.siz >> 1;
        uint32_t src_row = timg.address + ((uls >> 2) << timg.siz >> 1) + bytes_per_row * (ult >> 2);
        uint32_t rows = 1 + (lrt >> 2) - (ult >> 2), words = (lrs >> 2) - (uls >> 2) + 1;
        uint32_t dst_row = (tile.tmem << 3) & (tmem_bytes - 1), xor_mask = 0;
        for (uint32_t r = 0; r < rows && r < 64; r++) {
            uint32_t src = src_row, dst = dst_row;
            for (uint32_t w = 0; w < words && w < tmem_bytes / 8; w++) {
                for (uint32_t i = 0; i < 8; i++) {
                    uint32_t a = ((dst + i) ^ xor_mask) & (tmem_bytes - 1);
                    tmem[a] = rd8(src + (i & 1));
                    tmem_known[a] = true;
                }
                src += 2;
                dst = (dst + 8) & (tmem_bytes - 1);
            }
            dst_row = (dst_row + (tile.line << 5)) & (tmem_bytes - 1);
            src_row += bytes_per_row;
            xor_mask ^= 4;
        }
    }

    // RT64's sample size (State::updateDrawCall...) and hash for a rectangle drawn with tile `t`. `rect` is the
    // G_TEXRECT's three commands, or null for one the widescreen HUD made; `texels` is null for a flipped one.
    void texture_rect(uint32_t t, const uint32_t* rect, const RectTexels* texels) {
        const RT64::LoadTile& tile = tiles[t];
        if (tile.fmt == fmt_ci && tile.siz == siz_8b && rect != nullptr && title_scale > 0 && title_enabled && tile.line != 0) {
            title_rect(tile, rect);
            return;
        }
        if (!enabled.load(std::memory_order_relaxed) || tile.fmt != fmt_i || (tile.siz != siz_4b && tile.siz != siz_8b) ||
            tile.line == 0) {
            return;
        }
        bool clamp_s = tile.masks == 0 || (tile.cms & 2);
        bool clamp_t = tile.maskt == 0 || (tile.cmt & 2);
        int tile_w = clamp_s ? std::max((tile.lrs - tile.uls + 4) / 4, 1) : 0xFFFF;
        int tile_h = clamp_t ? std::max((tile.lrt - tile.ult + 4) / 4, 1) : 0xFFFF;
        int mask_w = tile.masks > 0 ? 1 << tile.masks : 0xFFFF;
        int mask_h = tile.maskt > 0 ? 1 << tile.maskt : 0xFFFF;
        uint16_t width = (uint16_t)std::min(tile_w, mask_w);
        uint16_t height = (uint16_t)std::min(tile_h, mask_h);
        if (width > 1024 || height > 1024 || RT64::TMEMHasher::requiresRawTMEM(tile, width, height, 0)) {
            return;
        }
        flush();
        uint32_t row_bytes = tile.line << 3;
        uint32_t start = tile.tmem << 3;
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < (uint32_t)((width << tile.siz) + 1) / 2; x++) {
                if (!tmem_known[(start + y * row_bytes + x) & (tmem_bytes - 1)]) {
                    return;
                }
            }
        }
        uint64_t hash = RT64::TMEMHasher::hash(tmem, tile, width, height, 0, RT64::TMEMHasher::CurrentHashVersion);
        bool make = seen.insert(hash).second;
        Glyph glyph;
        if (texels != nullptr && glyph_box(tile, *texels, width, height, glyph)) {
            std::vector<Glyph>& glyphs = atlas_glyphs[hash];
            if (glyphs.size() < max_glyphs && std::find(glyphs.begin(), glyphs.end(), glyph) == glyphs.end()) {
                glyphs.push_back(glyph);
                make = true;
            }
        }
        if (make && !pending.contains(hash)) {
            pending[hash] = { width, height, read_texels(tile, width, height) };
        }
    }

    // The texels a rectangle draws, point sampled, clipped to the texture.
    bool glyph_box(const RT64::LoadTile& tile, RectTexels r, uint32_t width, uint32_t height, Glyph& glyph) const {
        if (((othermode_h >> 20) & 3) == 2) {  // copy mode: the lower right edge is inclusive, and dsdx 4 a texel
            r.lrx += 1.0f;
            r.lry += 1.0f;
            r.dsdx /= 4.0f;
        }
        float s0 = r.s - tile.uls / 4.0f, t0 = r.t - tile.ult / 4.0f;
        float s1 = s0 + (r.lrx - r.ulx) * r.dsdx, t1 = t0 + (r.lry - r.uly) * r.dtdy;
        int x0 = std::max((int)std::floor(std::min(s0, s1)), 0);
        int x1 = std::min((int)std::ceil(std::max(s0, s1)), (int)width);
        int y0 = std::max((int)std::floor(std::min(t0, t1)), 0);
        int y1 = std::min((int)std::ceil(std::max(t0, t1)), (int)height);
        if (x0 >= x1 || y0 >= y1) {
            return false;
        }
        glyph = { (uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1 };
        return true;
    }

    // The texels, read back the way the RDP samples them: odd rows have their 32-bit halves swapped in TMEM.
    std::vector<float> read_texels(const RT64::LoadTile& tile, uint32_t width, uint32_t height) const {
        std::vector<float> src((size_t)width * height);
        uint32_t row_bytes = tile.line << 3;
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t a = (tile.tmem << 3) + y * row_bytes + (tile.siz == siz_4b ? x / 2 : x);
                if (y & 1) {
                    a ^= 4;
                }
                uint8_t b = tmem[a & (tmem_bytes - 1)];
                src[y * width + x] = tile.siz == siz_4b ? ((x & 1) ? (b & 0xF) : (b >> 4)) / 15.0f : b / 255.0f;
            }
        }
        return src;
    }

    void make_texture(uint64_t hash, uint32_t width, uint32_t height, const std::vector<float>& src);
    void title_rect(const RT64::LoadTile& tile, const uint32_t* rect);

    void walk(uint32_t addr, int depth) {
        if (depth > 12) {
            return;
        }
        uint32_t p = phys(addr);
        for (int n = 0; n < 50000; n++, p += 8) {
            uint32_t w0 = rd32(p), w1 = rd32(p + 4);
            switch (w0 >> 24) {
            case 0xDF:  // G_ENDDL
                return;
            case 0xDE:  // G_DL
                if (w0 & 0x00010000) {  // branch
                    p = phys(w1) - 8;
                } else {
                    walk(w1, depth + 1);
                }
                break;
            case 0xDB:  // G_MOVEWORD
                if (((w0 >> 16) & 0xFF) == 0x06) {
                    segments[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFF;
                }
                break;
            case 0xFD:  // G_SETTIMG
                timg.fmt = (w0 >> 21) & 7;
                timg.siz = (w0 >> 19) & 3;
                timg.width = (w0 & 0xFFF) + 1;
                timg.address = phys(w1);
                break;
            case 0xF5: {  // G_SETTILE
                RT64::LoadTile& tile = tiles[(w1 >> 24) & 7];
                tile.fmt = (w0 >> 21) & 7;
                tile.siz = (w0 >> 19) & 3;
                tile.line = (w0 >> 9) & 0x1FF;
                tile.tmem = w0 & 0x1FF;
                tile.palette = (w1 >> 20) & 0xF;
                tile.cmt = (w1 >> 18) & 3;
                tile.maskt = (w1 >> 14) & 0xF;
                tile.shiftt = (w1 >> 10) & 0xF;
                tile.cms = (w1 >> 8) & 3;
                tile.masks = (w1 >> 4) & 0xF;
                tile.shifts = w1 & 0xF;
                break;
            }
            case 0xF2: {  // G_SETTILESIZE
                RT64::LoadTile& tile = tiles[(w1 >> 24) & 7];
                tile.uls = (w0 >> 12) & 0xFFF;
                tile.ult = w0 & 0xFFF;
                tile.lrs = (w1 >> 12) & 0xFFF;
                tile.lrt = w1 & 0xFFF;
                break;
            }
            case 0xF3:  // G_LOADBLOCK
                load_block((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
                break;
            case 0xF4:  // G_LOADTILE
                forget_tmem();
                break;
            case 0xF0:  // G_LOADTLUT
                load_tlut((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
                break;
            case 0xE3: {  // G_SETOTHERMODE_H
                uint32_t len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
                uint32_t mask = (uint32_t)(((1ull << len) - 1) << shift);
                othermode_h = (othermode_h & ~mask) | (w1 & mask);
                break;
            }
            case 0xEF:  // G_RDPSETOTHERMODE
                othermode_h = w0 & 0x00FFFFFF;
                break;
            case 0xE4:  // G_TEXRECT
            case 0xE5: {  // G_TEXRECTFLIP
                uint32_t full[4] = { w0, w1, rd32(p + 12), rd32(p + 20) };
                RectTexels texels = { ((w1 >> 12) & 0xFFF) / 4.0f, (w1 & 0xFFF) / 4.0f, ((w0 >> 12) & 0xFFF) / 4.0f,
                                      (w0 & 0xFFF) / 4.0f, (int16_t)(full[2] >> 16) / 32.0f, (int16_t)full[2] / 32.0f,
                                      (int16_t)(full[3] >> 16) / 1024.0f, (int16_t)full[3] / 1024.0f };
                bool flip = (w0 >> 24) == 0xE5;
                texture_rect((w1 >> 24) & 7, flip ? nullptr : full, flip ? nullptr : &texels);
                p += 16;
                break;
            }
            case RT64_EXTENDED_OPCODE:
                switch (w0 & 0xFFFFFF) {
                case G_EX_TEXRECT_V1: {  // what the widescreen HUD turns race rectangles into
                    uint32_t xy0 = rd32(p + 8), xy1 = rd32(p + 12), st = rd32(p + 16), d = rd32(p + 20);
                    RectTexels texels = { (int16_t)(xy0 >> 16) / 4.0f, (int16_t)xy0 / 4.0f, (int16_t)(xy1 >> 16) / 4.0f,
                                          (int16_t)xy1 / 4.0f, (int16_t)(st >> 16) / 32.0f, (int16_t)st / 32.0f,
                                          (int16_t)(d >> 16) / 1024.0f, (int16_t)d / 1024.0f };
                    texture_rect(w1 & 7, nullptr, &texels);
                    p += 16;
                    break;
                }
                case G_EX_SETSCISSORALIGN_V1:
                    p += 16;
                    break;
                case G_EX_FILLRECT_V1:
                case G_EX_SETVIEWPORT_V1:
                case G_EX_SETSCISSOR_V1:
                case G_EX_SETRECTALIGN_V1:
                case G_EX_SETVIEWPORTALIGN_V1:
                case G_EX_MATRIXGROUP_V1:
                case G_EX_EDITGROUPBYADDRESS_V1:
                case G_EX_VERTEX_V1:
                case G_EX_MATRIX_FLOAT_V1:
                case G_EX_SETVERTEXSEGMENT_V1:
                    p += 8;
                    break;
                case G_EX_SETRDRAMEXTENDED_V1:
                    extended_rdram = w1 & 1;
                    break;
                }
                break;
            }
        }
    }
};

float cubic(float x) {
    // Catmull-Rom.
    x = std::fabs(x);
    if (x < 1.0f) {
        return 1.5f * x * x * x - 2.5f * x * x + 1.0f;
    }
    if (x < 2.0f) {
        return -0.5f * x * x * x + 2.5f * x * x - 4.0f * x + 2.0f;
    }
    return 0.0f;
}

// Uncompressed RGBA8 DDS with a full mip chain, so RT64 can filter the text when it is drawn smaller. Colour is
// averaged weighted by alpha, so a smaller level doesn't pull black from transparent texels into the edges.
bool write_dds(const std::filesystem::path& path, std::vector<uint8_t> rgba, uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    while ((width >> levels) > 0 || (height >> levels) > 0) {
        levels++;
    }
    uint32_t header[32]{};
    header[0] = 0x20534444;  // "DDS "
    header[1] = 124;
    header[2] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000;  // caps, height, width, pitch, pixel format, mip count
    header[3] = height;
    header[4] = width;
    header[5] = width * 4;
    header[7] = levels;
    header[19] = 32;
    header[20] = 0x1 | 0x40;  // alpha, RGB
    header[22] = 32;
    header[23] = 0x000000FF;
    header[24] = 0x0000FF00;
    header[25] = 0x00FF0000;
    header[26] = 0xFF000000;
    header[27] = 0x1000 | 0x400000 | 0x8;  // texture, mipmap, complex
    // Written aside and moved into place: a texture made again replaces one RT64 may be reading.
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    FILE* f = fopen(tmp.string().c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    fwrite(header, sizeof(header), 1, f);
    uint32_t w = width, h = height;
    for (uint32_t level = 0; level < levels; level++) {
        fwrite(rgba.data(), 1, (size_t)w * h * 4, f);
        uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
        std::vector<uint8_t> next((size_t)nw * nh * 4);
        for (uint32_t y = 0; y < nh; y++) {
            for (uint32_t x = 0; x < nw; x++) {
                uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                const uint8_t* in[4] = { &rgba[(y0 * w + x0) * 4], &rgba[(y0 * w + x1) * 4], &rgba[(y1 * w + x0) * 4],
                                         &rgba[(y1 * w + x1) * 4] };
                uint8_t* o = &next[(y * nw + x) * 4];
                uint32_t alpha = in[0][3] + in[1][3] + in[2][3] + in[3][3];
                for (uint32_t c = 0; c < 3; c++) {
                    uint32_t sum = in[0][c] * in[0][3] + in[1][c] * in[1][3] + in[2][c] * in[2][3] + in[3][c] * in[3][3];
                    o[c] = alpha == 0 ? 0 : (uint8_t)((sum + alpha / 2) / alpha);
                }
                o[3] = (uint8_t)((alpha + 2) / 4);
            }
        }
        rgba = std::move(next);
        w = nw;
        h = nh;
    }
    bool ok = ferror(f) == 0;
    fclose(f);
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, path, ec);
    }
    if (!ok || ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// An atlas's glyphs, beside its texture: a line of "x0 y0 x1 y1" each.
std::filesystem::path glyphs_path(uint64_t hash) {
    char name[32];
    snprintf(name, sizeof(name), "%016" PRIx64 ".glyphs", hash);
    return pack_dir / name;
}

void write_glyphs(uint64_t hash, const std::vector<Glyph>& glyphs) {
    std::string text;
    for (const Glyph& g : glyphs) {
        text += std::to_string(g.x0) + " " + std::to_string(g.y0) + " " + std::to_string(g.x1) + " " +
                std::to_string(g.y1) + "\n";
    }
    if (FILE* f = fopen(glyphs_path(hash).string().c_str(), "wb")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

void read_glyphs(uint64_t hash) {
    FILE* f = fopen(glyphs_path(hash).string().c_str(), "rb");
    if (f == nullptr) {
        return;
    }
    std::vector<Glyph>& glyphs = atlas_glyphs[hash];
    unsigned x0, y0, x1, y1;
    while (fscanf(f, "%u %u %u %u", &x0, &y0, &x1, &y1) == 4) {
        if (x0 < x1 && y0 < y1 && x1 <= 1024 && y1 <= 1024) {
            glyphs.push_back({ (uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1 });
        }
    }
    fclose(f);
}

void write_database(const std::filesystem::path& pack_dir, const std::vector<uint64_t>& in_pack) {
    std::string json = "{\n  \"configuration\": { \"autoPath\": \"rt64\", \"configurationVersion\": 3, \"hashVersion\": " +
                       std::to_string(RT64::TMEMHasher::CurrentHashVersion) +
                       ", \"defaultOperation\": \"preload\", \"defaultShift\": \"none\" },\n  \"textures\": [";
    for (size_t i = 0; i < in_pack.size(); i++) {
        char entry[96];
        snprintf(entry, sizeof(entry), "%s\n    { \"hashes\": { \"rt64\": \"%016" PRIx64 "\" }, \"path\": \"\" }",
                 i == 0 ? "" : ",", in_pack[i]);
        json += entry;
    }
    json += "\n  ]\n}\n";
    std::filesystem::path tmp = pack_dir / "rt64.json.tmp";
    FILE* f = fopen(tmp.string().c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    fwrite(json.data(), 1, json.size(), f);
    fclose(f);
    std::error_code ec;
    std::filesystem::rename(tmp, pack_dir / "rt64.json", ec);
}

void Rdp::make_texture(uint64_t hash, uint32_t width, uint32_t height, const std::vector<float>& src) {
    float peak = *std::max_element(src.begin(), src.end());
    if (peak == 0.0f) {
        return;
    }
    // The box each texel's upscale reads within: its glyph's (the first drawn, where two overlap), or the whole atlas.
    const std::vector<Glyph>& glyphs = atlas_glyphs[hash];
    std::vector<Glyph> box((size_t)width * height, Glyph{ 0, 0, (uint16_t)width, (uint16_t)height });
    for (auto g = glyphs.rbegin(); g != glyphs.rend(); ++g) {
        for (uint32_t y = g->y0; y < g->y1; y++) {
            std::fill(&box[y * width + g->x0], &box[y * width + g->x1], *g);
        }
    }

    uint32_t out_w = width * scale, out_h = height * scale;
    std::vector<uint8_t> rgba((size_t)out_w * out_h * 4);
    uint8_t colour = (uint8_t)std::lround(peak * 255.0f);
    auto texel = [&](const Glyph& g, int x, int y) {
        int cx = std::clamp(x, (int)g.x0, (int)g.x1 - 1), cy = std::clamp(y, (int)g.y0, (int)g.y1 - 1);
        float v = src[cy * width + cx];
        return (cx == x && cy == y) || v >= solid * peak ? v : 0.0f;
    };
    for (uint32_t oy = 0; oy < out_h; oy++) {
        float sy = (oy + 0.5f) / scale - 0.5f;
        int y0 = (int)std::floor(sy);
        for (uint32_t ox = 0; ox < out_w; ox++) {
            float sx = (ox + 0.5f) / scale - 0.5f;
            int x0 = (int)std::floor(sx);
            const Glyph& g = box[(oy / scale) * width + ox / scale];
            float v = 0.0f;
            for (int dy = -1; dy <= 2; dy++) {
                float wy = cubic(sy - (y0 + dy));
                for (int dx = -1; dx <= 2; dx++) {
                    v += wy * cubic(sx - (x0 + dx)) * texel(g, x0 + dx, y0 + dy);
                }
            }
            v = std::clamp((v / peak - 0.5f) * contrast + 0.5f, 0.0f, 1.0f) * peak;
            if (src[(oy / scale) * width + ox / scale] == 0.0f) {
                v = 0.0f;
            }
            uint8_t i = (uint8_t)std::lround(v * 255.0f);
            uint8_t* o = &rgba[((size_t)oy * out_w + ox) * 4];
            o[0] = o[1] = o[2] = i == 0 ? 0 : colour;
            o[3] = i;
        }
    }

    char name[32];
    snprintf(name, sizeof(name), "%016" PRIx64 ".dds", hash);
    if (!write_dds(pack_dir / name, std::move(rgba), out_w, out_h)) {
        fprintf(stderr, "[hires font] can't write %s\n", (pack_dir / name).string().c_str());
        return;
    }
    if (std::find(in_pack.begin(), in_pack.end(), hash) == in_pack.end()) {
        in_pack.push_back(hash);
    }
    found.push_back(hash);
    write_glyphs(hash, glyphs);
}

void Rdp::title_rect(const RT64::LoadTile& tile, const uint32_t* rect) {
    if (title_cutting.load(std::memory_order_acquire)) {
        return;
    }
    uint32_t w0 = rect[0], w1 = rect[1], st = rect[2], d = rect[3];
    int dsdx = (int16_t)(d >> 16), dtdy = (int16_t)d;  // s5.10
    if (dsdx != 4 << 10 || std::abs(dtdy) != 1 << 10) {  // copy mode, a texel to a pixel
        return;
    }
    TitleStrip strip;
    strip.ulx = ((w1 >> 12) & 0xFFF) / 4.0f;
    strip.uly = (w1 & 0xFFF) / 4.0f;
    strip.lrx = ((w0 >> 12) & 0xFFF) / 4.0f + 1.0f;  // inclusive in copy mode
    strip.lry = (w0 & 0xFFF) / 4.0f + 1.0f;
    if (strip.lrx > title_width || strip.lry > title_height) {
        return;
    }
    uint32_t tlut = othermode_h & (3u << 14);  // G_MDSFT_TEXTLUT, as RT64's OtherMode::textLUT
    int width = std::max((tile.lrs - tile.uls + 4) / 4, 1), height = std::max((tile.lrt - tile.ult + 4) / 4, 1);
    if (tlut == 0 || width > 1024 || height > 1024 ||
        RT64::TMEMHasher::requiresRawTMEM(tile, width, height, tlut)) {
        return;
    }
    flush();
    uint32_t row_bytes = tile.line << 3, start = tile.tmem << 3;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (!tmem_known[(start + y * row_bytes + x) & (tmem_bytes - 1)]) {
                return;
            }
        }
    }
    strip.hash = RT64::TMEMHasher::hash(tmem, tile, width, height, tlut, RT64::TMEMHasher::CurrentHashVersion);
    if (title_seen.count(strip.hash) != 0) {
        return;
    }
    strip.width = width;
    strip.height = height;
    strip.s0 = (int16_t)(st >> 16) / 32.0f - tile.uls / 4.0f;
    strip.t0 = (int16_t)st / 32.0f - tile.ult / 4.0f;
    strip.dt = dtdy / 1024.0f;

    for (int v = 0; v < height; v++) {
        for (int u = 0; u < width; u++) {
            float x = strip.screen_x(u + 0.5f), y = strip.screen_y(v + 0.5f);
            if (x < strip.ulx || x >= strip.lrx || y < strip.uly || y >= strip.lry) {
                continue;
            }
            uint32_t a = start + v * row_bytes + u;
            if (v & 1) {
                a ^= 4;
            }
            uint32_t e = (tmem_bytes / 2 + tmem[a & (tmem_bytes - 1)] * 8) & (tmem_bytes - 1);
            if (!tmem_known[e] || !tmem_known[e + 1]) {
                return;  // the palette was loaded by an earlier task
            }
            uint32_t c = (tmem[e] << 8) | tmem[e + 1];  // RGBA 5551
            strip.texels.push_back({ (int)x, (int)y, { channel(c, 0), channel(c, 1), channel(c, 2) } });
        }
    }
    if (strip.texels.empty()) {
        return;
    }
    if (title_rgba.empty()) {
        title_pending.push_back(std::move(strip));  // the patch is unpacked once the whole title is drawn
    } else {
        found_title |= cut_title_strip(strip);
    }
}

// The strip, if it is the title, cut from title_rgba into the pack.
bool cut_title_strip(const TitleStrip& strip) {
    int pic_w = title_width * title_scale, pic_h = title_height * title_scale;
    auto pixel = [&](float x, float y) {
        int px = std::clamp((int)std::floor(x * title_scale), 0, pic_w - 1);
        int py = std::clamp((int)std::floor(y * title_scale), 0, pic_h - 1);
        return &title_rgba[((size_t)py * pic_w + px) * 4];
    };

    // The strip as the game draws it against the picture shrunk back down: the mean of each texel's block.
    float diff = 0.0f;
    for (const TitleTexel& t : strip.texels) {
        float sum[3] = {};
        for (int j = 0; j < title_scale; j++) {
            for (int i = 0; i < title_scale; i++) {
                const uint8_t* p = pixel(t.x + (i + 0.5f) / title_scale, t.y + (j + 0.5f) / title_scale);
                for (int k = 0; k < 3; k++) {
                    sum[k] += p[k];
                }
            }
        }
        for (int k = 0; k < 3; k++) {
            diff += std::fabs(sum[k] / (title_scale * title_scale) - t.rgb[k]);
        }
    }
    diff /= strip.texels.size() * 3;
    title_seen.insert(strip.hash);
    if (diff > title_match) {
        fprintf(stderr, "[hires title] strip at (%g,%g) %016" PRIx64 " differs from the picture by %.1f, left alone\n", strip.ulx,
                strip.uly, strip.hash, diff);
        return false;
    }
    if (!title_original.empty()) {
        for (const TitleTexel& t : strip.texels) {
            std::copy(t.rgb, t.rgb + 3, &title_original[((size_t)t.y * title_width + t.x) * 3]);
        }
    }

    // Each texel gets the part of the picture it covers on screen. A flipped strip needs RT64 to start its high
    // resolution UV on the texel each pixel shows (RT64_FLIPPED_RECT_HIGH_RES), or it lands a row off.
    uint32_t out_w = strip.width * title_scale, out_h = strip.height * title_scale;
    std::vector<uint8_t> rgba((size_t)out_w * out_h * 4);
    for (uint32_t oy = 0; oy < out_h; oy++) {
        for (uint32_t ox = 0; ox < out_w; ox++) {
            const uint8_t* p = pixel(strip.screen_x((ox + 0.5f) / title_scale), strip.screen_y((oy + 0.5f) / title_scale));
            uint8_t* o = &rgba[((size_t)oy * out_w + ox) * 4];
            o[0] = p[0];
            o[1] = p[1];
            o[2] = p[2];
            o[3] = 255;
        }
    }
    char name[32];
    snprintf(name, sizeof(name), "%016" PRIx64 ".dds", strip.hash);
    if (!write_dds(title_dir / "pack" / name, std::move(rgba), out_w, out_h)) {
        fprintf(stderr, "[hires title] can't write %s\n", name);
        return false;
    }
    title_in_pack.push_back(strip.hash);
    return true;
}

// The patch's arithmetic, as tools/title_patch.py has it.
uint64_t splitmix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int median_predict(int a, int b, int c) {
    if (c >= std::max(a, b)) {
        return std::min(a, b);
    }
    if (c <= std::min(a, b)) {
        return std::max(a, b);
    }
    return a + b - c;
}

// title_rgba from the patch and the title as the game drew it, or false if the patch was made for another picture.
bool unpack_title(const std::vector<uint8_t>& original) {
    uint64_t seed = 0xCBF29CE484222325ull;  // FNV-1a
    for (uint8_t b : original) {
        seed = (seed ^ b) * 0x100000001B3ull;
    }
    if (splitmix(seed ^ 0x5449544C45434B31ull) != title_check) {
        return false;
    }
    std::vector<uint8_t> packed(title_patch.begin() + title_patch_header, title_patch.end());
    for (size_t i = 0; i < packed.size(); i++) {
        packed[i] ^= (uint8_t)(splitmix(seed + (i / 8 + 1) * 0x9E3779B97F4A7C15ull) >> (8 * (i % 8)));
    }
    int pic_w = title_width * title_scale, pic_h = title_height * title_scale;
    size_t plane = (size_t)pic_w * pic_h;
    std::vector<uint8_t> v(plane * 3);
    size_t got = ZSTD_decompress(v.data(), v.size(), packed.data(), packed.size());
    if (ZSTD_isError(got) || got != v.size()) {
        fprintf(stderr, "[hires title] the patch doesn't unpack\n");
        return false;
    }
    for (int p = 0; p < 3; p++) {
        uint8_t* q = &v[plane * p];
        for (int y = 0; y < pic_h; y++) {
            for (int x = 0; x < pic_w; x++) {
                int a = x > 0 ? q[y * pic_w + x - 1] : 0, b = y > 0 ? q[(y - 1) * pic_w + x] : 0;
                int c = x > 0 && y > 0 ? q[(y - 1) * pic_w + x - 1] : 0;
                q[y * pic_w + x] = (uint8_t)(q[y * pic_w + x] + median_predict(a, b, c));
            }
        }
    }
    // A bilinear upscale of the original with integer weights, sampled at pixel centres, plus the difference.
    int n = 2 * title_scale;
    auto axis = [&](int i, int size, int& i0, int& i1, int& w) {
        int f = 2 * i + 1 - title_scale;
        i0 = f >= 0 ? f / n : -((-f + n - 1) / n);
        w = f - i0 * n;
        i1 = std::clamp(i0 + 1, 0, size - 1);
        i0 = std::clamp(i0, 0, size - 1);
    };
    title_rgba.assign(plane * 4, 255);
    for (int y = 0; y < pic_h; y++) {
        int y0, y1, wy;
        axis(y, title_height, y0, y1, wy);
        for (int x = 0; x < pic_w; x++) {
            int x0, x1, wx;
            axis(x, title_width, x0, x1, wx);
            size_t i = (size_t)y * pic_w + x;
            int g = v[plane + i] - 128;
            int r[3] = { v[i] - 128 + g, g, v[plane * 2 + i] - 128 + g };
            for (int k = 0; k < 3; k++) {
                auto o = [&](int oy, int ox) { return (int)original[((size_t)oy * title_width + ox) * 3 + k]; };
                int top = o(y0, x0) * (n - wx) + o(y0, x1) * wx, bottom = o(y1, x0) * (n - wx) + o(y1, x1) * wx;
                int base = (top * (n - wy) + bottom * wy + n * n / 2) / (n * n);
                title_rgba[i * 4 + k] = (uint8_t)(base + r[k]);
            }
        }
    }
    return true;
}

// The title as the strips this task drew while the patch is still packed show it, or empty if they don't cover it.
std::vector<uint8_t> pending_title(const std::vector<TitleStrip>& pending) {
    std::vector<uint8_t> original((size_t)title_width * title_height * 3);
    std::vector<bool> covered((size_t)title_width * title_height);
    size_t count = 0;
    for (const TitleStrip& strip : pending) {
        for (const TitleTexel& t : strip.texels) {
            size_t i = (size_t)t.y * title_width + t.x;
            count += !covered[i];
            covered[i] = true;
            std::copy(t.rgb, t.rgb + 3, &original[i * 3]);
        }
    }
    if (count < covered.size()) {
        return {};  // another screen, or the title with a strip whose palette came in an earlier task
    }
    return original;
}

// The patch unpacked against the whole title and the strips that drew it cut from it.
bool cut_pending_title(const std::vector<TitleStrip>& pending, const std::vector<uint8_t>& original) {
    auto began = std::chrono::steady_clock::now();
    if (!unpack_title(original)) {
        fprintf(stderr, "[hires title] a full screen of strips that isn't the patch's title, left alone\n");
        for (const TitleStrip& strip : pending) {
            title_seen.insert(strip.hash);
        }
        return false;
    }
    bool found = false;
    for (const TitleStrip& strip : pending) {
        found |= cut_title_strip(strip);
    }
    fprintf(stderr, "[hires title] unpacked the patch against the title the game drew and cut it, %.0f ms\n",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
    return found;
}

void register_pack() {
    std::vector<std::filesystem::path> packs;
    if (enabled) {
        packs.push_back(pack_dir);
    }
    if (title_enabled && title_scale > 0) {
        packs.push_back(title_dir / "pack");
    }
    recompui::renderer::set_builtin_texture_packs(packs);
}

// The title as the game draws it, for title_patch.py.
void write_original() {
    std::FILE* f = std::fopen((title_dir / "original.ppm").string().c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", title_width, title_height);
    std::fwrite(title_original.data(), 1, title_original.size(), f);
    std::fclose(f);
}

// Strips added to the title's pack: RT64 reloads it.
void title_cut() {
    fprintf(stderr, "[hires title] %zu strip(s) in the pack\n", title_in_pack.size());
    write_database(title_dir / "pack", title_in_pack);
    register_pack();
    if (!title_original.empty()) {
        write_original();
    }
}

// hires_title/title.png in the app folder, or failing that assets/title.patch: the pack is cut afresh each launch.
void init_hires_title() {
    title_dir = recompui::file::get_app_folder_path() / "hires_title";
    std::error_code ec;
    std::filesystem::remove_all(title_dir / "pack", ec);
    int w = 0, h = 0, channels = 0;
    if (uint8_t* data = stbi_load((title_dir / "title.png").string().c_str(), &w, &h, &channels, 4)) {
        int scale = w / title_width;
        if (scale >= 2 && scale <= 8 && w == title_width * scale && h == title_height * scale) {
            title_rgba.assign(data, data + (size_t)w * h * 4);
            title_original.assign((size_t)title_width * title_height * 3, 0);
            title_scale = scale;
            fprintf(stderr, "[hires title] hires_title/title.png, %dx%d, %dx\n", w, h, scale);
        } else {
            fprintf(stderr, "[hires title] hires_title/title.png is %dx%d, not 320x220 times 2 to 8\n", w, h);
        }
        stbi_image_free(data);
    } else {
        std::filesystem::path path = recompui::file::get_asset_path("title.patch");
        std::FILE* f = std::fopen(path.string().c_str(), "rb");
        if (f == nullptr) {
            fprintf(stderr, "[hires title] no %s\n", path.string().c_str());
            return;
        }
        std::fseek(f, 0, SEEK_END);
        title_patch.resize(std::max(std::ftell(f), 0L));
        std::fseek(f, 0, SEEK_SET);
        size_t got = std::fread(title_patch.data(), 1, title_patch.size(), f);
        std::fclose(f);
        uint32_t version = 0, scale = 0, size = 0;
        if (got == title_patch.size() && got >= title_patch_header && std::memcmp(title_patch.data(), "R49TITLE", 8) == 0) {
            std::memcpy(&version, &title_patch[8], 4);
            std::memcpy(&scale, &title_patch[12], 4);
            std::memcpy(&title_check, &title_patch[16], 8);
            std::memcpy(&size, &title_patch[24], 4);
        }
        if (version != 1 || scale < 2 || scale > 8 || size != got - title_patch_header) {
            fprintf(stderr, "[hires title] %s isn't a title patch\n", path.string().c_str());
            title_patch.clear();
            return;
        }
        title_scale = (int)scale;
    }
    std::filesystem::create_directories(title_dir / "pack", ec);
    write_database(title_dir / "pack", title_in_pack);
}

} // namespace

void rush2049::init_hires_font() {
    std::filesystem::path root = recompui::file::get_app_folder_path() / "hires_font";
    pack_dir = root / pack_version;
    std::error_code ec;
    // Textures from an older upscale.
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (entry.path().filename() != pack_version) {
            std::filesystem::remove_all(entry.path(), ec);
        }
    }
    std::filesystem::create_directories(pack_dir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(pack_dir, ec)) {
        if (entry.path().extension() == ".dds") {
            uint64_t hash = std::strtoull(entry.path().stem().string().c_str(), nullptr, 16);
            seen.insert(hash);
            in_pack.push_back(hash);
            read_glyphs(hash);
        }
    }
    write_database(pack_dir, in_pack);
    init_hires_title();
    register_pack();
}

void rush2049::set_hires_font(bool on) {
    enabled = on;
    register_pack();
}

void rush2049::set_hires_title(bool on) {
    title_enabled = on;
    register_pack();
}

extern "C" void rush2049_hires_font_scan(uint8_t* rdram, uint32_t task_addr) {
    if ((!enabled.load(std::memory_order_relaxed) && !(title_enabled && title_scale > 0)) || pack_dir.empty()) {
        return;
    }
    gpr task = (gpr)(int32_t)task_addr;
    if (MEM_W(0x00, task) != 1) {  // M_GFXTASK
        return;
    }
    static Rdp rdp;
    rdp.rdram = rdram;
    rdp.found.clear();
    rdp.pending.clear();
    rdp.extended_rdram = false;
    rdp.found_title = false;
    rdp.title_pending.clear();
    rdp.forget_tmem();
    rdp.walk((uint32_t)MEM_W(0x30, task), 0);
    for (const auto& [hash, atlas] : rdp.pending) {
        rdp.make_texture(hash, atlas.width, atlas.height, atlas.texels);
    }
    if (!rdp.title_pending.empty()) {
        std::vector<uint8_t> original = pending_title(rdp.title_pending);
        if (!original.empty()) {
            title_cutting.store(true, std::memory_order_relaxed);
            std::thread([pending = std::move(rdp.title_pending), original = std::move(original)] {
                if (cut_pending_title(pending, original)) {
                    title_cut();
                }
                title_cutting.store(false, std::memory_order_release);
            }).detach();
        }
    }
    if (rdp.found_title) {
        title_cut();
    }
    if (!rdp.found.empty()) {
        write_database(pack_dir, in_pack);
        register_pack();
        fprintf(stderr, "[hires font] %zu texture(s) made, %zu in the pack\n", rdp.found.size(), in_pack.size());
    }
}
