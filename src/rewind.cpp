// Rewind, as in Braid: hold the Rewind input in a single-player race and the race runs backwards, every car, the
// clock, the laps and the ghost being recorded, until you let go and drive on from there.
//
// The race is saved in the frame loop (thread 7) just before it draws a race frame: there the frame's logic is done,
// and thread 5 is between two ticks of the race (when it isn't, the frame is skipped). What is saved is RAM from the
// end of the code up, minus the parts that belong to something other than the race: the OS's message queues, the
// controller and pak state, the audio and music threads' memory, the scheduler and graphics tasks, the vertex
// buffers, the Z-buffer and the framebuffers, and the timestamp the game measures each tick against. Those were found
// by watching which part of a frame writes each word: the tick, the frame's logic, its drawing, or another thread.
// Watch more than one track: a word can be something else's in a race and the race's elsewhere. Stunt's first arena
// animates a scene node from the frame's logic, stepping its model number (0x80130EA8) through the animation and to
// none (0xFFFF) at its end; with that word not saved, a rewind put the node back in the scene with no model, and
// drawing it (func_8009C8F0) read the model table out of bounds and crashed.
// Nor leave out part of one thing: func_800C0AC0 plays a keyframed animation, the checkered flags over the start line
// among them, from a state whose time (0x80138A44) was saved and whose keyframe and the rest (0x80138A4C-0x80138A5C)
// were not, so while Rewind was held the time went back and the keyframe kept going, and the flags juddered.
//
// A copy of that memory (the shadow) holds it as of the last frame saved. Each frame, the 64-byte pieces that changed
// since are stored with their old contents as that frame's record, and the shadow is updated. A frame while Rewind is
// held first puts back everything that changed since the shadow (the tick that ran from the restored state), then
// applies the newest record, which takes RAM and the shadow one frame further back. The records are capped at
// history_cap bytes, the oldest dropped first. A frame in which no race time passed (paused, or no tick since the last
// frame above 60 fps) is neither saved nor stepped back, so the race rewinds at the speed it was played.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>  // TEMP trace
#include <cstring>
#include <deque>
#include <vector>

#include "recomp.h"
#include "recompinput/profiles.h"
#include "rush2049_game.h"
#include "rush2049_hooks.h"

namespace {

constexpr uint32_t game_state_addr = 0x801174B4;  // the frame loop's state bits; 0x00400000 in a race
constexpr uint32_t race_clock_addr = 0x801543CC;  // float, written by the tick
constexpr uint32_t sound_count_addr = 0x80110254; // the sound manager's count of sounds started (not saved)
constexpr uint32_t sound_shift_addr = 0x801460F8; // a sound's ID is that count shifted left by this, or a slot
constexpr uint32_t saved_lo = 0x8010FD80;         // the end of the main code segment
constexpr uint32_t saved_hi = 0x80800000;
constexpr size_t history_cap = 128 * 1024 * 1024;

struct Range {
    uint32_t lo, hi;
};

// Not the race's (see the top of the file), in address order.
constexpr Range not_saved[] = {
    { 0x80110240, 0x80110290 },  // music: the playing song
    { 0x80111958, 0x80111968 },  // the retrace the last frame started at, the tick's timing
    { 0x80114730, 0x80114734 },  // frames in flight
    { 0x80116178, 0x8011617C },
    { 0x801161CC, 0x801161D0 },
    { 0x80118E30, 0x80118E34 },
    { 0x8011EACC, 0x8011EAD0 },
    { 0x80124EE0, 0x80124F90 },  // timing, with the tick's timestamp at 0x80124F70
    { 0x80125338, 0x80126128 },  // matrices for the display list
    { 0x8012A0E8, 0x8012AC28 },  // the same
    { 0x8012E608, 0x8012E6D4 },
    { 0x8013FED0, 0x8013FED4 },
    { 0x801403C0, 0x80140414 },
    { 0x80142728, 0x80143CF8 },  // the sound thread, its queues, command pool and tables
    { 0x80143F58, 0x80143F68 },  // the sound manager's table of sounds wanting a voice
    { 0x80143FFC, 0x80144E00 },  // its lists of free, playing and waiting sounds, then the controllers and paks
    { 0x801460C8, 0x80146200 },  // queues
    { 0x80149438, 0x8014943C },  // display list pointer
    { 0x80149780, 0x80149870 },  // queues
    { 0x80149B10, 0x80149DA4 },
    { 0x8014A11C, 0x8014A128 },
    { 0x8014A248, 0x8014A24C },
    { 0x80150BA0, 0x80150BD0 },
    { 0x80150F18, 0x80150F60 },  // boot queue messages
    { 0x80151AE0, 0x80151AE4 },
    { 0x80152730, 0x801527F0 },  // queues
    { 0x80153E68, 0x80153F18 },  // queue
    { 0x80154378, 0x80154390 },  // queue
    { 0x8015439C, 0x801543A0 },  // its message
    { 0x80155238, 0x80155290 },  // audio and OS settings
    { 0x80156BA8, 0x80156D00 },  // thread 8's queue, the graphics task
    { 0x8015B240, 0x8015B268 },  // graphics buffers
    { 0x8015F72C, 0x8015F7A0 },
    { 0x801613F0, 0x8017A640 },  // vertex buffers, queue
    { 0x8017AEC0, 0x8019D4C0 },  // Z-buffer
    { 0x801A6000, 0x80218A00 },  // audio, then the sound manager's sounds
    { 0x807BB000, 0x80800000 },  // framebuffers
};

std::atomic<bool> enabled{ true };
bool tick_running = false;     // thread 5 is inside a tick
std::vector<Range> saved;      // what is saved, as offsets into RDRAM
std::vector<uint8_t> shadow;   // the saved memory as of the last frame saved, indexed like RDRAM
bool have_shadow = false;
uint32_t shadow_sounds = 0;    // the sound count as of the shadow
uint32_t stepped_sounds = 0;   // the sound count as of the last step back
struct Frame {
    std::vector<uint8_t> changes;  // { offset, length, old contents } for each piece that changed
    uint32_t sounds;               // the sound count as of those old contents
};
std::deque<Frame> history;     // newest last
size_t history_bytes = 0;
uint32_t frames_back = 0;     // frames stepped back since Rewind was pressed

uint32_t read_word(const uint8_t* mem, uint32_t addr) {
    return *(const uint32_t*)(mem + (addr - 0x80000000u));
}

bool rewind_held() {
    return recompinput::profiles::get_game_input_held(0, recompinput::GameInput::REWIND) || rush2049::scripted_rewind();
}

void forget() {
    history.clear();
    history_bytes = 0;
    have_shadow = false;
}

// Whether n bytes at a and b are the same. The web build's memcmp (musl's) compares a byte at a time, and the 6 MB
// compared every race frame took 5 ms of a phone's frame loop that way; 32 bytes at a time it vectorizes.
bool same(const uint8_t* a, const uint8_t* b, uint32_t n) {
    uint32_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t x[4], y[4];
        memcpy(x, a + i, sizeof(x));
        memcpy(y, b + i, sizeof(y));
        if (((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) != 0) {
            return false;
        }
    }
    for (; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// Calls f(offset, length) for each piece of saved memory that differs from the shadow.
template <typename F>
void for_each_change(const uint8_t* rdram, F f) {
    constexpr uint32_t block = 4096, piece = 64;
    for (Range r : saved) {
        for (uint32_t p = r.lo; p < r.hi;) {
            uint32_t end = std::min(p + block, r.hi);
            if (same(rdram + p, shadow.data() + p, end - p)) {
                p = end;
                continue;
            }
            for (; p < end; p += piece) {
                uint32_t n = std::min(piece, end - p);
                if (!same(rdram + p, shadow.data() + p, n)) {
                    f(p, n);
                }
            }
        }
    }
}

void save_frame(uint8_t* rdram) {
    std::vector<uint8_t> record;
    for_each_change(rdram, [&](uint32_t p, uint32_t n) {
        size_t at = record.size();
        record.resize(at + 8 + n);
        memcpy(record.data() + at, &p, 4);
        memcpy(record.data() + at + 4, &n, 4);
        memcpy(record.data() + at + 8, shadow.data() + p, n);
        memcpy(shadow.data() + p, rdram + p, n);
    });
    record.shrink_to_fit();
    history_bytes += record.size();
    history.push_back({ std::move(record), shadow_sounds });
    shadow_sounds = read_word(rdram, sound_count_addr);
    while (history_bytes > history_cap) {
        history_bytes -= history.front().changes.size();
        history.pop_front();
    }
}

void step_back(uint8_t* rdram) {
    for_each_change(rdram, [&](uint32_t p, uint32_t n) { memcpy(rdram + p, shadow.data() + p, n); });
    stepped_sounds = read_word(rdram, sound_count_addr);
    if (history.empty()) {
        return;  // as far back as it goes: stay there
    }
    const std::vector<uint8_t>& record = history.back().changes;
    for (size_t at = 0; at < record.size();) {
        uint32_t p, n;
        memcpy(&p, record.data() + at, 4);
        memcpy(&n, record.data() + at + 4, 4);
        memcpy(rdram + p, record.data() + at + 8, n);
        memcpy(shadow.data() + p, record.data() + at + 8, n);
        at += 8 + n;
    }
    history_bytes -= record.size();
    shadow_sounds = history.back().sounds;
    history.pop_back();
    frames_back++;
}

// The sound manager stays live, and the race knows its sounds only by the ID the manager handed out. A sound started
// in the stretch that was rewound, or by a tick while Rewind was held, is one the race no longer knows of. One-shots
// end by themselves, but a loop plays or waits for a voice for the rest of the race, owner or not, and the race starts
// another when it gets there again: with Rewind used a lot they filled the manager's 80 sounds, and the next sound the
// race started (func_80092278 takes one off the free list unchecked) crashed. The manager's count of sounds started
// isn't saved, and each frame saved keeps its value, so when Rewind is let go every loop started between the frame it
// went back to and the last step back is ended; those the ticks since started are the race's again.
//
// One waiting for a voice is made one that times out, and the sound thread frees it at its next update: only flags
// change, no links, so it doesn't matter where the sound thread was stopped. One that has a voice is stopped the way
// the race stops a sound (func_80091C04, which queues the sound thread a command that stops the voice and frees the
// sound): the sound thread counts a timeout only while a sound waits, and frees a playing one only when its voice
// ends, which a looping voice never does, so a tire squeal started while Rewind was held played to the end of the race.
// The IDs are gathered first: the stop waits for the sound manager's lock, and the sound thread can change the lists
// while it waits.
int release_orphaned_sounds(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t playing = 0x80144028, waiting = 0x80144C58;
    const uint32_t shift = read_word(rdram, sound_shift_addr) & 31;
    std::vector<uint32_t> to_stop;
    int released = 0;
    for (uint32_t head : { playing, waiting }) {
        for (uint32_t node = read_word(rdram, head), n = 0; node != 0 && n < 256; node = read_word(rdram, node), n++) {
            const uint32_t id = read_word(rdram, node + 0xC);
            const uint32_t started = id >> shift;
            // The count goes back to 1 when it runs out, which takes days of play.
            const bool lost = stepped_sounds >= shadow_sounds ? started >= shadow_sounds && started < stepped_sounds
                                                              : started >= shadow_sounds || started < stepped_sounds;
            if (!lost || rdram[(node + 0x19 - 0x80000000u) ^ 3] == 0) {
                continue;
            }
            if (head == playing) {
                to_stop.push_back(id);
            } else {
                rdram[(node + 0x19 - 0x80000000u) ^ 3] = 0;
                const float expired = -100.0f;  // past the -10 at which the sound thread frees a sound that timed out
                memcpy(rdram + (node + 0x20 - 0x80000000u), &expired, 4);
            }
            released++;
        }
    }
    if (!to_stop.empty()) {
        // As a call from the frame loop at this point: the callee may change any register the hook's function holds.
        const recomp_context saved = *ctx;
        for (uint32_t id : to_stop) {
            ctx->r4 = (gpr)(int32_t)id;
            LOOKUP_FUNC(0x80091C04)(rdram, ctx);
        }
        *ctx = saved;
    }
    return released;
}

}

void rush2049::set_rewind(bool on) {
    enabled = on;
}

extern "C" void rush2049_rewind_tick_begin(uint8_t* rdram) {
    tick_running = true;
}

extern "C" void rush2049_rewind_tick_end(uint8_t* rdram) {
    tick_running = false;
}

extern "C" void rush2049_rewind_frame_begin(uint8_t* rdram) {
    if (read_word(rdram, game_state_addr) != 0x00400000 && have_shadow) {
        forget();
    }
}

extern "C" void rush2049_rewind_frame_draw(uint8_t* rdram, recomp_context* ctx) {
    if (read_word(rdram, game_state_addr) != 0x00400000) {
        return;  // menus draw here too
    }
    if (!enabled || rush2049::split_screen()) {
        if (have_shadow) {
            forget();
        }
        return;
    }
    if (tick_running) {
        return;
    }
    if (!have_shadow) {
        if (saved.empty()) {
            uint32_t lo = saved_lo;
            for (Range r : not_saved) {
                saved.push_back({ lo - 0x80000000u, r.lo - 0x80000000u });
                lo = r.hi;
            }
            if (lo < saved_hi) {
                saved.push_back({ lo - 0x80000000u, saved_hi - 0x80000000u });
            }
            shadow.resize(saved_hi - 0x80000000u);
        }
        for (Range r : saved) {
            memcpy(shadow.data() + r.lo, rdram + r.lo, r.hi - r.lo);
        }
        shadow_sounds = read_word(rdram, sound_count_addr);
        have_shadow = true;
        return;
    }
    if (read_word(rdram, race_clock_addr) == read_word(shadow.data(), race_clock_addr)) {
        return;  // no race time has passed
    }
    if (std::getenv("REWIND_TRACE")) {  // TEMP diagnostic: unsaved words that change on most held frames
        static std::vector<uint8_t> prev, first;
        static std::vector<uint32_t> count;
        static int frames = 0;
        static bool was_held = false;
        static std::vector<Range> unsaved;
        if (unsaved.empty()) {
            unsaved.push_back({ 0x8002C5E0, 0x80086A50 });
            for (Range r : not_saved) {
                if (r.lo != 0x8017AEC0 && r.lo != 0x807BB000) unsaved.push_back(r);
            }
        }
        bool held = rewind_held();
        if (prev.empty()) {
            prev.assign(rdram, rdram + 0x00800000);
            count.assign(0x00200000, 0);
        }
        if (held != was_held) {
            if (frames > 20) {
                fprintf(stderr, "[trace] %s ended after %d frames\n", was_held ? "HELD" : "race", frames);
                int shown = 0;
                for (Range r : unsaved) {
                    for (uint32_t a = r.lo; a < r.hi; a += 4) {
                        uint32_t w = (a - 0x80000000u) / 4;
                        if (count[w] * 4 >= (uint32_t)frames * 3 && shown++ < 200) {
                            uint32_t f = *(uint32_t*)(first.data() + w * 4), l = *(uint32_t*)(rdram + w * 4);
                            float ff, lf;
                            memcpy(&ff, &f, 4);
                            memcpy(&lf, &l, 4);
                            fprintf(stderr, "[trace]  %08X %u/%d %08X->%08X (%g->%g)\n", a, count[w], frames, f, l, ff, lf);
                        }
                    }
                }
            }
            std::fill(count.begin(), count.end(), 0);
            first.assign(rdram, rdram + 0x00800000);
            frames = 0;
            was_held = held;
        }
        if (first.empty()) first.assign(rdram, rdram + 0x00800000);
        frames++;
        for (Range r : unsaved) {
            for (uint32_t a = r.lo; a < r.hi; a += 4) {
                uint32_t o = a - 0x80000000u;
                if (*(uint32_t*)(rdram + o) != *(uint32_t*)(prev.data() + o)) count[o / 4]++;
            }
        }
        memcpy(prev.data(), rdram, 0x00800000);
    }
    if (rewind_held()) {
        step_back(rdram);
        return;
    }
    if (frames_back > 0) {
        int released = release_orphaned_sounds(rdram, ctx);
        fprintf(stderr, "[rewind] back %u frames, %zu frames (%zu MB) left, %d lost sounds released\n", frames_back,
            history.size(), history_bytes >> 20, released);
        frames_back = 0;
    }
    save_frame(rdram);
}
