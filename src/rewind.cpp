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
// The display lists are the frame's drawing, not the race: two of 0x4B00 bytes from 0x80124FE8, the frame loop
// building one (func_800A4934 picks it by the index at 0x8012E6C0) before the hook. Only the parts seen written were
// left out at first, and a Ghost Race's HUD draws past them: a step back put an older frame's commands into the list
// being drawn, so the radar flickered, and at the start of the race the menus' commands drew a garbled box.
// The other way, the race's words lie between the sound side's: the lap box's timers and times (0x80142740 and
// 0x80142770, one per car) and each car's lap start (0x80142AC8), which func_800D2458 sets when a car makes a lap and
// takes from the time of the next. Left out, the tick that ran from a frame just before the line while Rewind was held
// made the lap again and moved its start up to the line, and crossing the line again counted a lap of a few
// milliseconds.
//
// The game's timer (func_80001350 to func_800015BC) lies below the saved memory: a count of 60ths of a second that
// always runs (0x8002EB64), the count its game time is measured from, and the count at which its countdown runs out,
// a Ghost Race's time limit (func_80001578 is the time left). Each frame keeps how far the count is from those two,
// and a step back puts them that far from the count again, as the game's own pause moves them along with it. Left
// out, the countdown kept counting down while Rewind was held.
//
// A copy of that memory (the shadow) holds it as of the last frame saved. Each frame, the 64-byte pieces that changed
// since are stored with their old contents as that frame's record, and the shadow is updated. A frame while Rewind is
// held first puts back everything that changed since the shadow (the tick that ran from the restored state), then
// applies the newest record, which takes RAM and the shadow one frame further back. The records are capped at
// history_cap bytes, the oldest dropped first. A frame in which no race time passed (paused, or no tick since the last
// frame above 60 fps) is neither saved nor stepped back, so the race rewinds at the speed it was played. Until the
// race clock first moves (the fly-by and the count), the shadow follows the race and nothing is recorded, so the
// oldest frame Rewind reaches is the green light, not the fly-by's first frame.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
constexpr uint32_t car_sounds_addr = 0x80140420;  // each car's looping sounds, 0x54 bytes a car (func_800E0050)
constexpr uint32_t skid_sounds_addr = 0x80140640; // each car's skid squeal, 0x14 bytes a car (func_800E05F0)
constexpr uint32_t car_sounds_on_addr = 0x8010FFC4;  // a byte a car: its sounds are started (func_800D5E64)
constexpr uint32_t sound_on_addr = 0x8010FFC0;    // a byte: the race plays car sounds at all
constexpr uint32_t cars_addr = 0x8014A250;        // the cars, 0x808 bytes each
constexpr uint32_t car_count_addr = 0x80152744;   // a byte: how many
constexpr uint32_t ticks_addr = 0x8002EB64;       // the timer's count of 60ths of a second, always running
constexpr uint32_t deadline_addr = 0x8002EB9C;    // the count at which its countdown runs out
constexpr uint32_t start_addr = 0x8002EBA0;       // the count its game time is measured from
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
    { 0x80124FE8, 0x8012E5E8 },  // the two display lists, 0x4B00 bytes each (func_800A4934)
    { 0x8012E608, 0x8012E6D4 },
    { 0x8013FED0, 0x8013FED4 },
    { 0x801403C0, 0x80140414 },
    { 0x80142728, 0x80142740 },  // the sound manager's lock, a message queue
    { 0x80142768, 0x8014276C },  // and its message, among the lap box's words
    { 0x801427A8, 0x80142AC8 },  // the sound thread's command queue and tables
    { 0x80142AD8, 0x80143CF8 },  // its command pool and tables
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
struct Timer {
    uint32_t since_start, to_deadline;  // in the timer's 60ths, from its count
};
Timer shadow_timer{};          // the timer as of the shadow
uint32_t start_clock = 0;      // the race clock in the first race frame
struct Frame {
    std::vector<uint8_t> changes;  // { offset, length, old contents } for each piece that changed
    uint32_t sounds;               // the sound count as of those old contents
    Timer timer;                   // and the timer
};
std::deque<Frame> history;     // newest last
size_t history_bytes = 0;
bool rewinding = false;       // Rewind is held and the race has been stepped back
uint32_t frames_back = 0;     // frames stepped back since Rewind was pressed
std::vector<uint32_t> released;  // the IDs of the sounds ended since then
struct Spared {
    uint32_t id;
    std::chrono::steady_clock::time_point since;
};
std::vector<Spared> spared;    // lost sounds left to play out (see release_lost_sounds)

uint32_t read_word(const uint8_t* mem, uint32_t addr) {
    return *(const uint32_t*)(mem + (addr - 0x80000000u));
}

void write_word(uint8_t* mem, uint32_t addr, uint32_t value) {
    *(uint32_t*)(mem + (addr - 0x80000000u)) = value;
}

uint8_t read_byte(const uint8_t* mem, uint32_t addr) {
    return mem[(addr - 0x80000000u) ^ 3];
}

uint16_t read_half(const uint8_t* mem, uint32_t addr) {
    return *(const uint16_t*)(mem + ((addr - 0x80000000u) ^ 2));
}

Timer read_timer(const uint8_t* rdram) {
    const uint32_t now = read_word(rdram, ticks_addr);
    return { now - read_word(rdram, start_addr), read_word(rdram, deadline_addr) - now };
}

void write_timer(uint8_t* rdram, Timer t) {
    const uint32_t now = read_word(rdram, ticks_addr);
    write_word(rdram, start_addr, now - t.since_start);
    write_word(rdram, deadline_addr, now + t.to_deadline);
}

bool rewind_held() {
    return recompinput::profiles::get_game_input_held(0, recompinput::GameInput::REWIND) || rush2049::scripted_rewind();
}

void forget() {
    history.clear();
    history_bytes = 0;
    have_shadow = false;
    rewinding = false;
    frames_back = 0;
    released.clear();
    spared.clear();
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
    history.push_back({ std::move(record), shadow_sounds, shadow_timer });
    shadow_sounds = read_word(rdram, sound_count_addr);
    shadow_timer = read_timer(rdram);
    while (history_bytes > history_cap) {
        history_bytes -= history.front().changes.size();
        history.pop_front();
    }
}

void step_back(uint8_t* rdram) {
    for_each_change(rdram, [&](uint32_t p, uint32_t n) { memcpy(rdram + p, shadow.data() + p, n); });
    write_timer(rdram, shadow_timer);
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
    shadow_timer = history.back().timer;
    write_timer(rdram, shadow_timer);
    history.pop_back();
    frames_back++;
}

// Each car's engine, two loops (samples 102 and 115) whose volumes cross-fade with its speed, is kept in four slots of
// { ID, volume, pitch, ... } after a word of its own, and func_800E0050 hands a slot's sound a new volume or pitch
// only when it differs from the one the slot kept. Those copies are the race's and go back with it, but the sound
// keeps what a tick while Rewind was held gave it: a layer the replayed stretch turned up stayed at that volume when
// the rewound race had it at 0, as the race, its copy already 0, never sent the 0. After a step back every slot in
// use gets the copies the game gives a slot that hasn't sent yet (-2.0), so the next tick sends the rewound race's
// volume and pitch.
void resend_car_sounds(uint8_t* rdram) {
    constexpr uint32_t unsent = 0xC0000000;  // -2.0f
    for (uint32_t car = 0; car < 8; car++) {
        for (uint32_t slot = car_sounds_addr + car * 0x54 + 4, n = 0; n < 4; slot += 0x14, n++) {
            if (read_word(rdram, slot) != 0xFFFFFFFF) {
                write_word(rdram, slot + 4, unsent);
                write_word(rdram, slot + 8, unsent);
            }
        }
    }
}

// The sound manager stays live, and the race knows its sounds only by the ID the manager handed out. A sound started
// in the stretch stepped back over, or by the tick that ran from the frame stepped back to, is one the race no longer
// knows of. Some end by themselves, but a loop plays or waits for a voice for the rest of the race, owner or not, and
// so does a sound the race ends itself: the squeal of a skid (sample 36) isn't a loop by the flag at +0x19, which comes
// from the sample's entry in the table at 0x8011F5CC, yet one started while Rewind was held played on through the race
// and the menus after it. With Rewind used a lot they filled the manager's 80 sounds, and the next sound the race
// started (func_80092278 takes one off the free list unchecked) crashed. The manager's count of sounds started isn't
// saved, and each frame saved keeps its value, so each step back ends every sound started between the frame it goes
// back to and the step. Ended only once Rewind was let go, they played on while it was held: a tick from a frame just
// before a skid started its squeal (func_800E05F0 starts one for a slot of -1), the step back took the slot back to
// -1, and the squeal of every skid the hold went back over played on, the dirt under the wheels too.
// Ending them all at every step back took the one-off sounds with them: the tick that runs from the frame just before
// a crash, an explosion or a landing starts its sound, and the step ended it before it was heard. So a lost sound that
// plays out by itself is spared: not a loop by the flag, and not held in the race's tables of sounds (held_lo to
// held_hi, below) as the tick left them, which hold the ones the race ends itself, the squeal among them. The tick
// from each frame crosses a moment once, so each one-off sound the rewind goes back over plays once, forwards, as the
// music does. A spared sound still playing after spared_for is ended all the same, in case one only the race would
// have ended is held somewhere else.
//
// One waiting for a voice is made one that times out, and the sound thread frees it at its next update: only flags
// change, no links, so it doesn't matter where the sound thread was stopped. One that has a voice is stopped the way
// the race stops a sound (func_80091C04, which queues the sound thread a command that stops the voice and frees the
// sound): the sound thread counts a timeout only while a sound waits, and frees a playing one only when its voice
// ends, which a looping voice never does. A sound stopped stays in the playing list until the sound thread has run
// the stop, so the IDs ended are kept until Rewind is let go, and each is ended once.
// The IDs are gathered first: the stop waits for the sound manager's lock, and the sound thread can change the lists
// while it waits.
constexpr uint32_t playing_head = 0x80144028, waiting_head = 0x80144C58;

// Calls f(list head, node, ID) for each sound the manager has, playing or waiting for a voice.
template <typename F>
void for_each_sound(const uint8_t* rdram, F f) {
    for (uint32_t head : { playing_head, waiting_head }) {
        for (uint32_t node = read_word(rdram, head), n = 0; node != 0 && n < 256; node = read_word(rdram, node), n++) {
            f(head, node, read_word(rdram, node + 0xC));
        }
    }
}

// As a call from the frame loop at this point: the callee may change any register the hook's function holds.
void call_game(uint8_t* rdram, recomp_context* ctx, uint32_t func, uint32_t arg) {
    const recomp_context saved = *ctx;
    ctx->r4 = (gpr)(int32_t)arg;
    LOOKUP_FUNC(func)(rdram, ctx);
    *ctx = saved;
}

constexpr auto spared_for = std::chrono::seconds(10);

// Ends a sound as above, the stop of a playing one added to to_stop for after the lists are walked.
void end_sound(uint8_t* rdram, uint32_t head, uint32_t node, uint32_t id, std::vector<uint32_t>& to_stop) {
    if (head == playing_head) {
        to_stop.push_back(id);
    } else {
        rdram[(node + 0x19 - 0x80000000u) ^ 3] = 0;
        const float expired = -100.0f;  // past the -10 at which the sound thread frees a sound that timed out
        memcpy(rdram + (node + 0x20 - 0x80000000u), &expired, 4);
    }
    released.push_back(id);
}

void stop_sounds(uint8_t* rdram, recomp_context* ctx, const std::vector<uint32_t>& to_stop) {
    for (uint32_t id : to_stop) {
        call_game(rdram, ctx, 0x80091C04, id);
    }
}

bool has(const std::vector<uint32_t>& v, uint32_t id) {
    return std::find(v.begin(), v.end(), id) != v.end();
}

bool is_spared(uint32_t id) {
    return std::any_of(spared.begin(), spared.end(), [&](const Spared& s) { return s.id == id; });
}

// Ends the sounds started from the count from up to the count to, but the ones kept and the ones that play out by
// themselves. tables is the race's tables of sounds as the tick left them.
void release_lost_sounds(uint8_t* rdram, recomp_context* ctx, uint32_t from, uint32_t to,
    const std::vector<uint32_t>& kept, const std::vector<uint8_t>& tables) {
    const uint32_t shift = read_word(rdram, sound_shift_addr) & 31;
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> to_stop;
    for_each_sound(rdram, [&](uint32_t head, uint32_t node, uint32_t id) {
        const uint32_t started = id >> shift;
        // The count goes back to 1 when it runs out, which takes days of play.
        const bool lost = to >= from ? started >= from && started < to : started >= from || started < to;
        if (!lost || has(released, id) || has(kept, id) || is_spared(id)) {
            return;
        }
        bool held = read_byte(rdram, node + 0x19) != 0;
        for (size_t at = 0; at + 4 <= tables.size() && !held; at += 4) {
            held = *(const uint32_t*)(tables.data() + at) == id;
        }
        fprintf(stderr, "[TEMP] lost %08X sample %u loop %u held %d\n", id, read_word(rdram, node + 0x34), read_byte(rdram, node + 0x19), (int)held);  // TEMP
        if (held) {
            end_sound(rdram, head, node, id, to_stop);
        } else {
            spared.push_back({ id, now });
        }
    });
    stop_sounds(rdram, ctx, to_stop);
}

// Forgets the spared sounds that have ended and ends the ones that have played for spared_for.
void end_old_spared(uint8_t* rdram, recomp_context* ctx) {
    if (spared.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> live, to_stop;
    for_each_sound(rdram, [&](uint32_t head, uint32_t node, uint32_t id) {
        live.push_back(id);
        for (const Spared& s : spared) {
            if (s.id == id && now - s.since >= spared_for && !has(released, id)) {
                fprintf(stderr, "[TEMP] spared %08X sample %u ended at %d s\n", id, read_word(rdram, node + 0x34), (int)spared_for.count());  // TEMP
                end_sound(rdram, head, node, id, to_stop);
            }
        }
    });
    for (const Spared& s : spared) if (!has(live, s.id)) fprintf(stderr, "[TEMP] spared %08X done after %.2f s\n", s.id, std::chrono::duration<double>(now - s.since).count());  // TEMP
    spared.erase(std::remove_if(spared.begin(), spared.end(),
        [&](const Spared& s) { return !has(live, s.id) || has(released, s.id); }), spared.end());
    stop_sounds(rdram, ctx, to_stop);
}

// The race holds each car's sounds by ID in tables of its own, and a sound the replayed stretch ended is one the race
// still holds when a step back takes it to before that end: the manager no longer has it, so the race sets the volume
// of nothing and starts no other while its slot says one plays. A squeal, a wall's grind or the grass under the wheels
// went silent from where Rewind reached the end of one until it was let go, and on past it until the race ended that
// sound itself: let go during a grind, it stayed silent for as long as the car stayed on the wall. And the engine is
// stopped with the car's other sounds when it crashes (func_800D5828) and started again under new IDs when it respawns
// (func_800D5E64), so a rewind back past a reset held the engine the crash stopped, and the respawn's was a lost sound.
// A held sound the manager no longer has is given back after every step back, and when Rewind is let go:
//  - Each car's slots of { ID, volume, pitch, ... } (func_800D54BC clears one): the engine's two loops (samples 102
//    and 115) and one more, the squeal of a skid (func_800E05F0), and for a player's car the three under its wheels
//    (func_800DFBA0), one at a time. The race starts any of them but the engine when it finds its slot clear, so such
//    a slot is cleared. The engine is started only by a respawn, so a car whose engine is lost gets the stop and start
//    a pause gives it: func_800D5524 ends its sounds and clears all of these, func_800D5E64 starts the engine.
//  - The grind of a wall (sample 19 at 0x80140AE0, func_800DEF68): contact puts the car's state (0x80140A08) at the
//    side it touches, 1 to 3, and its time (0x80140B10); 0.1 s on the state goes to 4 to 6 and the grind starts, and
//    only the end of the contact stops it and puts the state at 0. A lost grind's state goes back to 1 to 3 with a
//    contact that began long ago, so the next tick starts it.
//  - A sound a player's car starts and stops in a pair (sample 25 at 0x801407C0) on a state (car + 0x65C) that asks for
//    the start (1), plays (2) and asks for the stop (3): a lost one in state 2 goes back to 1.
// A tick while Rewind is held runs from the frame stepped back to and starts a sound for a slot cleared like that,
// which the next step back would end as a lost sound and clear again, a new start every frame. So when the frame
// stepped back to holds a lost sound where the tick just held one the manager still has, that one is kept and the
// race given its ID: the squeal, the grind and the engine play on through the rewind as one sound each.
constexpr uint32_t held_lo = 0x80140420, held_hi = 0x80140B14;  // all of the tables below
constexpr uint32_t wheel_sounds_addr = 0x801406C0; // 0x3C bytes for each of the first four cars
constexpr uint32_t pair_sounds_addr = 0x801407C0;  // a word a car
constexpr uint32_t grind_state_addr = 0x80140A08;  // a half a car
constexpr uint32_t grind_sounds_addr = 0x80140AE0; // a word a car
constexpr uint32_t grind_since_addr = 0x80140B10;  // a float a car

enum class Held { engine, slot, pair, grind };

void write_half(uint8_t* mem, uint32_t addr, uint16_t value) {
    *(uint16_t*)(mem + ((addr - 0x80000000u) ^ 2)) = value;
}

// Gives back the sounds the race holds that the manager no longer has, as above. before is the tables as the tick
// left them, before the step back (nullptr when Rewind is let go). Returns how many engines were started again, and
// adds the IDs the race was given to kept.
int restore_held_sounds(uint8_t* rdram, recomp_context* ctx, const std::vector<uint8_t>* before,
    std::vector<uint32_t>& kept) {
    if (read_byte(rdram, sound_on_addr) == 0) {
        return 0;
    }
    std::vector<uint32_t> live;
    for_each_sound(rdram, [&](uint32_t, uint32_t, uint32_t id) {
        if (std::find(released.begin(), released.end(), id) == released.end()) {
            live.push_back(id);
        }
    });
    auto is_live = [&](uint32_t id) { return std::find(live.begin(), live.end(), id) != live.end(); };
    // Whether the slot holds a lost sound, once given the one it held before the step back if that one isn't.
    auto lost = [&](uint32_t slot) {
        const uint32_t id = read_word(rdram, slot);
        if (id == 0xFFFFFFFF || is_live(id)) {
            return false;
        }
        const uint32_t was = before != nullptr ? *(const uint32_t*)(before->data() + (slot - held_lo)) : id;
        if (was != 0xFFFFFFFF && is_live(was)) {
            write_word(rdram, slot, was);
            kept.push_back(was);
            return false;
        }
        return true;
    };
    int restarted = 0;
    const uint32_t cars = std::min<uint32_t>(read_byte(rdram, car_count_addr), 8);
    for (uint32_t car = cars_addr, n = 0; n < cars; car += 0x808, n++) {
        const uint32_t index = read_half(rdram, car + 0x7C6);
        if (index >= 8 || read_byte(rdram, car_sounds_on_addr + index) == 0 || read_byte(rdram, car + 0x640) != 0) {
            continue;  // its sounds are stopped (a crash), and its respawn starts them
        }
        const uint32_t engine = car_sounds_addr + index * 0x54 + 4;
        std::vector<std::pair<uint32_t, Held>> slots = {
            { engine, Held::engine }, { engine + 0x14, Held::engine }, { engine + 0x3C, Held::slot },
            { skid_sounds_addr + index * 0x14, Held::slot }, { pair_sounds_addr + index * 4, Held::pair },
            { grind_sounds_addr + index * 4, Held::grind },
        };
        if (read_byte(rdram, car + 0x7CC) == 2 && index < 4) {  // a player's car
            for (uint32_t k = 0; k < 3; k++) {
                slots.push_back({ wheel_sounds_addr + index * 0x3C + k * 0x14, Held::slot });
            }
        }
        bool engine_lost = false;
        for (auto [slot, how] : slots) {
            if (!lost(slot)) {
                continue;
            }
            if (how == Held::engine) {
                engine_lost = true;
            } else if (how == Held::slot) {
                write_word(rdram, slot, 0xFFFFFFFF);
                for (uint32_t k = 4; k < 0x14; k += 4) {
                    write_word(rdram, slot + k, 0xC0000000);  // -2.0f, not sent
                }
            } else if (how == Held::pair) {
                write_word(rdram, slot, 0xFFFFFFFF);
                if (read_half(rdram, car + 0x65C) == 2) {
                    write_half(rdram, car + 0x65C, 1);
                }
            } else {
                write_word(rdram, slot, 0xFFFFFFFF);
                const uint32_t state = grind_state_addr + index * 2;
                const uint16_t side = read_half(rdram, state);
                if (side >= 4 && side <= 6) {
                    write_half(rdram, state, side - 3);
                    const float long_ago = -1e30f;
                    memcpy(rdram + (grind_since_addr + index * 4 - 0x80000000u), &long_ago, 4);
                }
            }
        }
        if (engine_lost) {
            call_game(rdram, ctx, 0x800D5524, car);
            call_game(rdram, ctx, 0x800D5E64, car);
            restarted++;
        }
    }
    return restarted;
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
        shadow_timer = read_timer(rdram);
        start_clock = read_word(rdram, race_clock_addr);
        have_shadow = true;
        return;
    }
    if (history.empty() && frames_back == 0 && read_word(shadow.data(), race_clock_addr) == start_clock) {
        // The race hasn't started: the shadow follows it, so the oldest frame is the first one with time on the
        // clock (see the top of the file).
        for_each_change(rdram, [&](uint32_t p, uint32_t n) { memcpy(shadow.data() + p, rdram + p, n); });
        shadow_sounds = read_word(rdram, sound_count_addr);
        shadow_timer = read_timer(rdram);
        return;
    }
    if (read_word(rdram, race_clock_addr) == read_word(shadow.data(), race_clock_addr)) {
        return;  // no race time has passed
    }
    end_old_spared(rdram, ctx);
    if (rewind_held()) {
        const uint32_t sounds = read_word(rdram, sound_count_addr);
        const std::vector<uint8_t> before(rdram + (held_lo - 0x80000000u), rdram + (held_hi - 0x80000000u));
        step_back(rdram);
        std::vector<uint32_t> kept;
        restore_held_sounds(rdram, ctx, &before, kept);
        release_lost_sounds(rdram, ctx, shadow_sounds, sounds, kept, before);
        resend_car_sounds(rdram);
        rewinding = true;
        return;
    }
    if (rewinding) {
        std::vector<uint32_t> kept;
        const int restarted = restore_held_sounds(rdram, ctx, nullptr, kept);
        fprintf(stderr, "[rewind] back %u frames, %zu frames (%zu MB) left, %zu lost sounds released, %d engines "
            "restarted\n", frames_back, history.size(), history_bytes >> 20, released.size(), restarted);
        rewinding = false;
        frames_back = 0;
        released.clear();
    }
    save_frame(rdram);
}
