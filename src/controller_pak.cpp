// Rush 2049's side of the Controller Pak. The pak itself is the runtime's (N64ModernRuntime's librecomp/src/pak.cpp):
// libultra's osPfs* over saves/controllerpak_<port>.mpk in the app folder, the `.mpk` format emulators use, in every
// port (controller_pak_ports in src/main.cpp's GameEntry). Each write is whole, and a file that is a symlink, as when
// saves are shared through a synced folder, has the file it points at replaced and stays a link.
//
// rush2049::controller_pak_banks (GameEntry::controller_pak_banks) answers the runtime each time the game initializes a
// port's pak: the "Controller Pak Size" option's 1 bank (Nintendo's own 32 KiB paks) or 62 (libultra's PFS_MAX_BANKS,
// about 1.9 MB), and 0 at Controller Accessory's Rumble Pak. The runtime moves a pak of the other size's files to a pak
// of the size picked, and keeps the size of one whose files would not fit. For a Rumble Pak in the slot it fails
// osPfsInitPak with PFS_ERR_DEVICE, as libultra's ID check does on one, after filling in queue and channel: the game
// then tries osMotorInit, and reads queue back, still zero meaning the call never reached a pak, so no pak at all.
//
// Ghosts: the game writes a Ghost Race's ghost to the pak only from its SAVE GHOST prompt, and its ghost select offers
// the ghosts read from the pak beside the one kept in RAM for the session. The game shows that prompt only when byte
// 0x801148E4 is set, which it does only when it loads a profile named "000F1CC5 EJK", a developer's (the name entry
// can type it). The Save Ghosts setting sets it before each check (rush2049_ghost_save_check, hooked in us.toml), but
// only for a player profile on a pak: the prompt names the ghost's file after the profile's file, and in Just Play
// there is none (its profile's file pointer is 0, and saving read through it). Original leaves the byte to the game.
// For each pak the prompt shows the ghost it already holds for the track, and asks before replacing it; the game's
// check for that only works for a player name of 6 characters, so with Save Ghosts on rush2049_ghost_prompt_match
// (us.toml) answers it instead.
//
// Rumble with a Controller Pak in (Controller Accessory at Both): the game probes each port with osPfsInitPak and only
// when that fails tries osMotorInit, and marks the port as holding a Rumble Pak. Its menus skip such a port's saves,
// so the probe still finds a Controller Pak, and only the game's reads about rumble are told there is a Rumble Pak too
// (rush2049_rumble_pak_in, hooked in us.toml, which lists them): the motor routines, the INSERT RUMBLE PAK NOW banner,
// the rumble item of its CONTROLS menu and its Rumble Pak screen. The motor routines pass the port's OSPfs, the
// Controller Pak's, to osMotorInit and osMotorStart. The runtime's versions only rewrite its status, queue, channel and
// activebank (to the same queue and channel), and drive the controller's rumble; the runtime's pak reads neither status
// nor activebank.

#include <string>

#include "recomp.h"
#include "rush2049_game.h"

int rush2049::controller_pak_banks(int) {
    if (rush2049::rumble_pak_selected()) {
        return 0;
    }
    return rush2049::large_controller_pak() ? 62 : 1;
}

// Whether a guest pointer is in RDRAM.
static bool in_rdram(uint32_t addr) {
    return addr >= 0x80000000u && addr < 0x80800000u;
}

extern "C" gpr rush2049_rumble_pak_in(gpr rumble_pak) {
    return rumble_pak != 0 || rush2049::both_paks_selected() ? 1 : 0;
}

extern "C" void rush2049_ghost_save_check(uint8_t* rdram) {
    if (!rush2049::save_ghosts()) {
        return;
    }
    // The chain the prompt's save (func_800CC040) reads for the file name: the player at 0x8014A160, its profile,
    // the profile's pak file at +0x08 and that file's record.
    uint32_t player = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8014A160);
    uint32_t profile = in_rdram(player) ? (uint32_t)MEM_W(0, (gpr)(int32_t)player) : 0;
    uint32_t file = in_rdram(profile) ? (uint32_t)MEM_W(0x08, (gpr)(int32_t)profile) : 0;
    uint32_t record = in_rdram(file) ? (uint32_t)MEM_W(0, (gpr)(int32_t)file) : 0;
    MEM_B(0, (gpr)(int32_t)0x801148E4) = in_rdram(record) ? 1 : 0;
}

// A pak file record's name, from +0x12: at most 16 characters, as a note holds.
static std::string record_name(uint8_t* rdram, uint32_t file) {
    uint32_t record = in_rdram(file) ? (uint32_t)MEM_W(0, (gpr)(int32_t)file) : 0;
    std::string name;
    for (int i = 0; in_rdram(record) && i < 16; i++) {
        char c = (char)MEM_B(0x12 + i, (gpr)(int32_t)record);
        if (c == 0) {
            break;
        }
        name += c;
    }
    return name;
}

extern "C" void rush2049_ghost_prompt_match(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2049::save_ghosts()) {
        return;
    }
    // The ghost's list entry (s0, its file at +0x04, track at +0x08, direction at +0x09) and the profile (s2, its file
    // at +0x08). The save names the ghost's file "%s%d%s": the profile's file name, the track + 1, and F, or B for a
    // reversed track; a note keeps 16 characters of it.
    uint32_t ghost = (uint32_t)ctx->r16;
    uint32_t profile = (uint32_t)ctx->r18;
    std::string expected = record_name(rdram, (uint32_t)MEM_W(0x08, (gpr)(int32_t)profile)) +
        std::to_string((int)MEM_B(0x08, (gpr)(int32_t)ghost) + 1) + (MEM_B(0x09, (gpr)(int32_t)ghost) != 0 ? "B" : "F");
    ctx->r2 = record_name(rdram, (uint32_t)MEM_W(0x04, (gpr)(int32_t)ghost)) == expected.substr(0, 16) ? 0 : 1;
}
