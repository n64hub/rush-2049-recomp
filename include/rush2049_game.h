#ifndef RUSH2049_GAME_H
#define RUSH2049_GAME_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rush2049 {
    inline const std::u8string program_id = u8"Rush2049Recompiled";
    inline const std::string program_name = "San Francisco Rush 2049: Recompiled";

    void register_overlays();
    // Mods (README, "Mods"): the patches library (src/register_patches.cpp), the functions the
    // executable exports to mods (src/mod_exports.cpp), and the recomp ROM rebuilt for hooking (src/code_loading.cpp).
    void register_patches();
    void register_mod_exports();
    std::vector<uint8_t> build_recomp_rom(std::span<const uint8_t> rom);
    void init_config();
    // The accessory in every controller: a Controller Pak (saves), a Rumble Pak (rumble, no saves), or both at once.
    bool rumble_pak_selected();
    bool both_paks_selected();
    // The "Controller Pak Size" setting: 62 banks (about 1.9 MB) rather than the original 32 KiB.
    bool large_controller_pak();
    // The "Save Ghosts" setting: the SAVE GHOST prompt after every finished Ghost Race, not only a developer's.
    bool save_ghosts();
    // Frames a second the game draws at the "Game Frame Rate" setting: 30, 60, or more with Match Framerate.
    // The frame rate the Game Frame Rate option asks for; 0 is Match Framerate (src/frame_rate.cpp).
    void set_game_frame_rate(uint32_t fps);
    uint32_t game_frame_rate();
    // The "Input Lag" setting: a frame is drawn as soon as the game queues it (Reduced), not on the next retrace.
    void set_reduced_input_lag(bool reduced);
    // Whether 2- and 4-player views widen with the window ("Split-Screen Aspect Ratio" at Expand).
    bool split_screen_expand();
    // Whether the menus' background is mirrored out to a wide window's edges ("Menu Aspect Ratio" at Expand).
    bool menu_expand();
    // --dump-dl: write the first display list the game submits after the given number of seconds (src/dl_dump.cpp).
    void start_dl_dump(const char* path, double after_seconds);
    // --dump-ram: write all of RDRAM (big-endian) to <prefix>N.bin at each of the comma-separated times in seconds.
    void start_ram_dump(const char* prefix, const char* times);
    // --input: whether the input script holds Rewind right now (src/main.cpp).
    bool scripted_rewind();
    // Rewind (src/rewind.cpp): the General tab's Rewind option.
    void set_rewind(bool enabled);
    // Whether the last frame the game drew was split screen (src/widescreen.cpp).
    bool split_screen();

    // Hi-res font (src/hires_font.cpp): set up the generated texture pack in the app folder, then turn it on or off.
    void init_hires_font();
    void set_hires_font(bool on);
    // Hi-res title screen (src/hires_font.cpp, set up by init_hires_font): the title cut from assets/title.patch.
    void set_hires_title(bool on);
    // Draw Distance and Level of Detail (src/draw_distance.cpp): how far the distance limits reach, and the LOD switch
    // distances, as multiples of the game's own; 1 is Original.
    void set_draw_distance(float scale);
    void set_level_of_detail(float scale);
}

#endif
