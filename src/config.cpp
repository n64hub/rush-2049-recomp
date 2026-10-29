// Config tabs: the stock tabs, plus the controller accessory, Controller Pak size and rewind options on the General
// tab, the game frame rate, hi-res font, split-screen aspect ratio and copying frames to RAM on the Graphics tab, and
// MusyX mixing on the Sound tab.

#include <algorithm>
#include <atomic>
#include <filesystem>

#include "rush2049_game.h"
#include "rush2049_hooks.h"
#include "musyx_hle.h"
#include "librecomp/config.hpp"
#include "recompinput/input_mapping.h"
#include "recompinput/players.h"
#include "recompui/config.h"
#include "recompui/renderer.h"
#include "util/file.h"
#include "ultramodern/ultramodern.hpp"

namespace {
    enum class Accessory {
        ControllerPak,
        RumblePak,
        Both,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> accessory_options = {
        {Accessory::ControllerPak, "Controller Pak"},
        {Accessory::RumblePak, "Rumble Pak"},
        {Accessory::Both, "Both"},
    };
    const std::string accessory_option = "accessory";

    enum class Controllers {
        PerPlayer,
        OnePlayer,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> controllers_options = {
        {Controllers::PerPlayer, "PerPlayer", "Per Player"},
        {Controllers::OnePlayer, "OnePlayer", "One Player"},
    };
    const std::string controllers_option = "controllers";

    enum class PakSize {
        Original,
        Large,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> pak_size_options = {
        {PakSize::Original, "Original", "Original (32 KiB)"},
        {PakSize::Large, "Large", "Large (1.9 MB)"},
    };
    const std::string pak_size_option = "pak_size";

    enum class Rewind {
        Off,
        On,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> rewind_options = {
        {Rewind::Off, "Off", "Off"},
        {Rewind::On, "On", "On"},
    };
    const std::string rewind_option = "rewind";

    enum class SaveGhosts {
        Original,
        On,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> save_ghosts_options = {
        {SaveGhosts::Original, "Original", "Original"},
        {SaveGhosts::On, "On", "On"},
    };
    const std::string save_ghosts_option = "save_ghosts";

    enum class GameFrameRate {
        Original,
        Sixty,
        MatchFramerate,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> game_frame_rate_options = {
        {GameFrameRate::Original, "Original", "30 FPS (Original)"},
        {GameFrameRate::Sixty, "60", "60 FPS"},
        {GameFrameRate::MatchFramerate, "MatchFramerate", "Match Framerate"},
    };
    const std::string game_frame_rate_option = "game_frame_rate";

    enum class InputLag {
        Reduced,
        Original,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> input_lag_options = {
        {InputLag::Reduced, "Reduced", "Reduced"},
        {InputLag::Original, "Original", "Original"},
    };
    const std::string input_lag_option = "input_lag";

    enum class HiresFont {
        Off,
        On,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> hires_font_options = {
        {HiresFont::Off, "Off", "Off"},
        {HiresFont::On, "On", "On"},
    };
    const std::string hires_font_option = "hires_font";

    enum class HiresTitle {
        Original,
        On,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> hires_title_options = {
        {HiresTitle::Original, "Original", "Original"},
        {HiresTitle::On, "On", "On"},
    };
    const std::string hires_title_option = "hires_title";

    // src/draw_distance.cpp says what each limit is.
    enum class DrawDistance {
        Original,
        Far,
        Farthest,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> draw_distance_options = {
        {DrawDistance::Original, "Original", "Original"},
        {DrawDistance::Far, "Far", "4x"},
        {DrawDistance::Farthest, "Farthest", "16x"},
    };
    const std::string draw_distance_option = "draw_distance";

    enum class LevelOfDetail {
        Original,
        High,
        Highest,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> level_of_detail_options = {
        {LevelOfDetail::Original, "Original", "Original"},
        {LevelOfDetail::High, "High", "High"},
        {LevelOfDetail::Highest, "Highest", "Highest"},
    };
    const std::string level_of_detail_option = "level_of_detail";

    // src/draw_distance.cpp says what the game culls by.
    enum class SceneryCulling {
        Original,
        Off,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> scenery_culling_options = {
        {SceneryCulling::Original, "Original", "Original"},
        {SceneryCulling::Off, "Off", "Off"},
    };
    const std::string scenery_culling_option = "scenery_culling";

    enum class SplitScreenRatio {
        Expand,
        Original,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> split_screen_ratio_options = {
        {SplitScreenRatio::Expand, "Expand", "Expand"},
        {SplitScreenRatio::Original, "Original", "Original"},
    };
    const std::string split_screen_ratio_option = "split_screen_ratio";
    // Read on the game thread with every display list.
    std::atomic<SplitScreenRatio> split_screen_ratio_setting{ SplitScreenRatio::Expand };

    enum class MenuRatio {
        Expand,
        Original,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> menu_ratio_options = {
        {MenuRatio::Expand, "Expand", "Expand"},
        {MenuRatio::Original, "Original", "Original"},
    };
    const std::string menu_ratio_option = "menu_ratio";
    // Read on the game thread with every display list.
    std::atomic<MenuRatio> menu_ratio_setting{ MenuRatio::Expand };

    enum class CopyFramesToRam {
        Off,
        Original,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> copy_frames_to_ram_options = {
        {CopyFramesToRam::Off, "Off", "Off"},
        {CopyFramesToRam::Original, "Original", "Original"},
    };
    const std::string copy_frames_to_ram_option = "copy_frames_to_ram";

    enum class MusyxMix {
        Original,
        HighQuality,
        OptionCount
    };
    const std::vector<recomp::config::ConfigOptionEnumOption> musyx_mix_options = {
        {MusyxMix::Original, "Original", "Original"},
        {MusyxMix::HighQuality, "HighQuality", "High Quality"},
    };
    const std::string musyx_mix_option = "musyx_mix";
}

bool rush2049::split_screen_expand() {
    return split_screen_ratio_setting.load(std::memory_order_relaxed) == SplitScreenRatio::Expand;
}

bool rush2049::menu_expand() {
    return menu_ratio_setting.load(std::memory_order_relaxed) == MenuRatio::Expand;
}

static Accessory accessory() {
    auto value = recompui::config::get_general_config().get_option_value(accessory_option);
    return static_cast<Accessory>(std::get<uint32_t>(value));
}

bool rush2049::rumble_pak_selected() {
    return accessory() == Accessory::RumblePak;
}

bool rush2049::both_paks_selected() {
    return accessory() == Accessory::Both;
}

bool rush2049::save_ghosts() {
    auto value = recompui::config::get_general_config().get_option_value(save_ghosts_option);
    return static_cast<SaveGhosts>(std::get<uint32_t>(value)) == SaveGhosts::On;
}

bool rush2049::large_controller_pak() {
    auto value = recompui::config::get_general_config().get_option_value(pak_size_option);
    return static_cast<PakSize>(std::get<uint32_t>(value)) == PakSize::Large;
}

// Keyboard defaults for driving. The stock defaults put the C buttons on the arrow keys, where a PC player
// reaching for Up/Left would hit Abort (C-Up) and Reverse (C-Left). Here the arrows are a second stick (steering
// and menus, like WASD), gas and brake stay on Space and Left Shift, and the C buttons move to a T/F/G/H diamond.
// Only defaults: saved bindings and the game's own CONTROLS screen still apply.
static void set_keyboard_defaults() {
    using recompinput::GameInput;
    using recompinput::InputField;
    recompinput::set_default_mapping_for_keyboard(GameInput::X_AXIS_NEG,
        { InputField::keyboard(SDL_SCANCODE_A), InputField::keyboard(SDL_SCANCODE_LEFT) });
    recompinput::set_default_mapping_for_keyboard(GameInput::X_AXIS_POS,
        { InputField::keyboard(SDL_SCANCODE_D), InputField::keyboard(SDL_SCANCODE_RIGHT) });
    recompinput::set_default_mapping_for_keyboard(GameInput::Y_AXIS_POS,
        { InputField::keyboard(SDL_SCANCODE_W), InputField::keyboard(SDL_SCANCODE_UP) });
    recompinput::set_default_mapping_for_keyboard(GameInput::Y_AXIS_NEG,
        { InputField::keyboard(SDL_SCANCODE_S), InputField::keyboard(SDL_SCANCODE_DOWN) });
    recompinput::set_default_mapping_for_keyboard(GameInput::C_UP, { InputField::keyboard(SDL_SCANCODE_T) });
    recompinput::set_default_mapping_for_keyboard(GameInput::C_LEFT, { InputField::keyboard(SDL_SCANCODE_F) });
    recompinput::set_default_mapping_for_keyboard(GameInput::C_DOWN, { InputField::keyboard(SDL_SCANCODE_G) });
    recompinput::set_default_mapping_for_keyboard(GameInput::C_RIGHT, { InputField::keyboard(SDL_SCANCODE_H) });
}

// Gamepad defaults for driving on a modern pad (Xbox, PlayStation, Switch Pro; any pad SDL knows as a game
// controller). The stock defaults are an N64 layout: gas and brake on the A and X face buttons, the triggers on
// R (shift up) and Z, and shift down on RB. Here the triggers are gas and brake, the bumpers shift, view moves
// to Y and Z to B. The face buttons keep gas and brake for menus, and the right stick stays the C buttons.
// Only defaults: saved bindings and the game's own CONTROLS screen still apply.
static void set_controller_defaults() {
    using recompinput::GameInput;
    using recompinput::InputField;
    recompinput::set_default_mapping_for_controller(GameInput::A, {
        InputField::controller_analog(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true),
        InputField::controller_digital(recompinput::SDL_CONTROLLER_BUTTON_SOUTH) });
    recompinput::set_default_mapping_for_controller(GameInput::B, {
        InputField::controller_analog(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true),
        InputField::controller_digital(recompinput::SDL_CONTROLLER_BUTTON_WEST) });
    recompinput::set_default_mapping_for_controller(GameInput::R,
        { InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) });
    recompinput::set_default_mapping_for_controller(GameInput::C_DOWN, {
        InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, true),
        InputField::controller_digital(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) });
    recompinput::set_default_mapping_for_controller(GameInput::L,
        { InputField::controller_digital(recompinput::SDL_CONTROLLER_BUTTON_NORTH) });
    recompinput::set_default_mapping_for_controller(GameInput::C_LEFT,
        { InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, false) });
    recompinput::set_default_mapping_for_controller(GameInput::Z,
        { InputField::controller_digital(recompinput::SDL_CONTROLLER_BUTTON_EAST) });
    recompinput::set_default_mapping_for_controller(GameInput::C_RIGHT,
        { InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, true) });
}

void rush2049::init_config() {
    set_keyboard_defaults();
    set_controller_defaults();

    std::filesystem::path app_dir = recompui::file::get_app_folder_path();
    if (!app_dir.empty()) {
        std::filesystem::create_directories(app_dir);
    }

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;
    recomp::config::Config& general = recompui::config::create_general_tab(general_options);
    general.add_enum_option(
        accessory_option,
        "Controller Accessory",
        "What is plugged into each controller. A real controller has one accessory slot, so the game was made for "
        "either saving or rumble."
        "<br/>"
        "<b>Controller Pak:</b> saves players, records and ghosts. "
        "<b>Rumble Pak:</b> rumble, but the game can't save. "
        "<b>Both:</b> saves, and rumble in races.",
        accessory_options,
        Accessory::Both
    );
    // Per Player is RecompFrontend's multiplayer mode: each player has a port of their own and each controller a
    // profile of its own, and a controller becomes the next player the first time it presses a button
    // (RecompFrontend-0003), or players are put in order with Assign Players (Controls page). Nothing of it is saved,
    // so players join again each session. One Player is its single-player mode: every controller and the keyboard on
    // port 1 through one shared profile. The Controls page reads the mode each time its tab opens, so switching needs
    // no restart.
    general.add_enum_option(
        controllers_option,
        "Controllers",
        "How controllers and the keyboard map to players."
        "<br/>"
        "<b>Per Player:</b> each controller is a player of its own, as on the console. The first controller to press a "
        "button is player 1, the next player 2, and so on, and the keyboard drives player 1. Assign Players on the "
        "Controls tab puts them in another order or gives the keyboard to another player. "
        "<b>One Player:</b> every controller and the keyboard drive player 1.",
        controllers_options,
        Controllers::PerPlayer
    );
    general.add_enum_option(
        pak_size_option,
        "Controller Pak Size",
        "How much each Controller Pak holds. A player profile takes 9 of a pak's pages, and a pak holds 16 files "
        "whatever its size."
        "<br/>"
        "<b>Original:</b> Nintendo's 32 KiB pak, 123 pages, about 13 profiles. "
        "<b>Large:</b> 62 banks, 7748 pages, the most libultra can address, so it never fills up. "
        "Emulators that only take 32 KiB paks can't read it."
        "<br/>"
        "Saves are moved to the new size the next time the game starts. A pak whose saves don't fit in 32 KiB "
        "stays large.",
        pak_size_options,
        PakSize::Large
    );
    general.add_enum_option(
        rewind_option,
        "Rewind",
        "Hold <recomp-color primary>Rewind</recomp-color> (Controls tab) in a single-player race and the race runs "
        "backwards: every car, the clock, the laps and the ghost. Let go to drive on from there. Goes back up to about "
        "a minute, and uses up to 128 MB of memory for it.",
        rewind_options,
        Rewind::On
    );
    general.add_enum_option(
        save_ghosts_option,
        "Save Ghosts",
        "Whether a Ghost Race you finish inside the time limit can be saved to the Controller Pak, to race against "
        "in later sessions as GHOST 2 to 4."
        "<br/>"
        "<b>On:</b> the game's own SAVE GHOST prompt comes up after every such race. "
        "<b>Original:</b> as the game shipped, a ghost lasts only until you quit, as GHOST 1 on the same track. "
        "The game saves one only for a developer's profile.",
        save_ghosts_options,
        SaveGhosts::On
    );
    // Called on load too.
    general.add_option_change_callback(controllers_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            recompinput::players::set_single_player_mode(
                static_cast<Controllers>(std::get<uint32_t>(cur_value)) == Controllers::OnePlayer);
        });
    general.add_option_change_callback(rewind_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2049::set_rewind(static_cast<Rewind>(std::get<uint32_t>(cur_value)) == Rewind::On);
        });

    recomp::config::Config& graphics = recompui::config::create_graphics_tab();
    graphics.add_enum_option(
        game_frame_rate_option,
        "Game Frame Rate",
        "How often the game draws a new frame. The original draws on every other screen refresh, 30 times a second. "
        "Car physics and timers follow real time, so a race runs at the same speed at any rate."
#ifdef __EMSCRIPTEN__
        " In a browser, 60 FPS draws 30 while the device can't keep 60 up, and tries 60 again now and then."
#endif
        "<br/>"
        "<b>Match Framerate:</b> a new frame for every frame <recomp-color primary>Framerate</recomp-color> above "
        "presents, so Display draws at the monitor's refresh rate and Manual at its value (60 at least).",
        game_frame_rate_options,
        GameFrameRate::Sixty
    );
    // Called on load too, and while the menu previews a change.
    graphics.add_option_change_callback(game_frame_rate_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            static constexpr uint32_t fps[] = { 30, 60, 0 };  // Original, 60, Match Framerate
            rush2049::set_game_frame_rate(fps[std::get<uint32_t>(cur_value)]);
        });

    graphics.add_enum_option(
        input_lag_option,
        "Input Lag",
        "<b>Reduced:</b> each frame is drawn as soon as the game has worked it out, so what you press shows on screen "
        "one frame sooner (17 ms at 60 fps). At 30 FPS (Original) Game Frame Rate it changes nothing."
        "<br/>"
        "<b>Original:</b> each frame waits for the next refresh before it is drawn, as on the console.",
        input_lag_options,
        InputLag::Reduced
    );
    graphics.add_option_change_callback(input_lag_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2049::set_reduced_input_lag(static_cast<InputLag>(std::get<uint32_t>(cur_value)) == InputLag::Reduced);
        });

    rush2049::init_hires_font();
    graphics.add_enum_option(
        hires_font_option,
        "Hi-res Font",
        "Draws the menu and HUD text from smooth, 4x upscaled copies of the game's fonts instead of the blocky "
        "originals. The copies are made from your ROM as each font first appears, and kept in the app folder."
        "<br/>"
        "Texture packs you enable take priority.",
        hires_font_options,
        HiresFont::On
    );
    graphics.add_option_change_callback(hires_font_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2049::set_hires_font(static_cast<HiresFont>(std::get<uint32_t>(cur_value)) == HiresFont::On);
        });

    graphics.add_enum_option(
        hires_title_option,
        "Hi-res Title Screen",
        "Draws the title screen from a 4x upscale of its picture (made with Real-ESRGAN) instead of the 240p original. "
        "The game rebuilds the upscale from its own picture and a patch that ships with it."
        "<br/>"
        "Texture packs you enable take priority.",
        hires_title_options,
        HiresTitle::On
    );
    graphics.add_option_change_callback(hires_title_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2049::set_hires_title(static_cast<HiresTitle>(std::get<uint32_t>(cur_value)) == HiresTitle::On);
        });

    graphics.add_enum_option(
        draw_distance_option,
        "Draw Distance",
        "How far away scenery is still drawn, as a multiple of the original distance. Farther scenery, like a building "
        "far down the road, appears sooner instead of popping in. Most of what the original leaves out is hidden "
        "behind something nearer anyway, so the difference shows only in places.",
        draw_distance_options,
        DrawDistance::Far
    );
    // Called on load too, and while the menu previews a change.
    graphics.add_option_change_callback(draw_distance_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            static constexpr float scale[] = { 1.0f, 4.0f, 16.0f };
            rush2049::set_draw_distance(scale[std::get<uint32_t>(cur_value)]);
        });

    graphics.add_enum_option(
        level_of_detail_option,
        "Level of Detail",
        "How far away models keep their detailed versions before the game swaps in simpler ones. The original "
        "simplifies them close to the car: curves go blocky and small details like traffic lights disappear."
        "<br/>"
        "<b>High:</b> four times the original distances. <b>Highest:</b> always the most detailed version.",
        level_of_detail_options,
        LevelOfDetail::High
    );
    // Called on load too, and while the menu previews a change.
    graphics.add_option_change_callback(level_of_detail_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            static constexpr float scale[] = { 1.0f, 4.0f, 1e30f };
            rush2049::set_level_of_detail(scale[std::get<uint32_t>(cur_value)]);
        });

    graphics.add_enum_option(
        scenery_culling_option,
        "Scenery Culling",
        "The original game leaves out the scenery it reckons can't be seen from the stretch of road you are on, so "
        "buildings, signs and roadside pieces pop in and out as you drive from one stretch into the next, some of "
        "them close to the car."
        "<br/>"
        "<b>Off:</b> all of the track's scenery is drawn wherever you are, and only <recomp-color primary>Draw "
        "Distance</recomp-color> decides how far.",
        scenery_culling_options,
        SceneryCulling::Off
    );
    // Called on load too, and while the menu previews a change.
    graphics.add_option_change_callback(scenery_culling_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2049::set_scenery_culling(static_cast<SceneryCulling>(std::get<uint32_t>(cur_value)) == SceneryCulling::Original);
        });

    graphics.add_enum_option(
        split_screen_ratio_option,
        "Split-Screen Aspect Ratio",
        "How the 2- and 4-player views fit a wide window. The original game splits a 4:3 screen, so at 16:9 the "
        "views sit inside a 4:3 area with bars at the sides."
        "<br/>"
        "<b>Expand:</b> each view widens to fill its share of the window, and its HUD moves with it. Needs "
        "<recomp-color primary>Aspect Ratio</recomp-color> at Expand and "
        "<recomp-color primary>HUD Placement</recomp-color> at something other than Original.",
        split_screen_ratio_options,
        SplitScreenRatio::Expand
    );
    graphics.add_option_change_callback(split_screen_ratio_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            split_screen_ratio_setting = static_cast<SplitScreenRatio>(std::get<uint32_t>(cur_value));
        });

    graphics.add_enum_option(
        menu_ratio_option,
        "Menu Aspect Ratio",
        "How the menus fit a wide window. The original game draws them on a 4:3 picture, so at 16:9 there are bars "
        "at the sides."
        "<br/>"
        "<b>Expand:</b> the menu stays where it is and the background picture is mirrored out to the window's edges "
        "(up to about 21:9). Needs <recomp-color primary>Aspect Ratio</recomp-color> at Expand.",
        menu_ratio_options,
        MenuRatio::Expand
    );
    graphics.add_option_change_callback(menu_ratio_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            menu_ratio_setting = static_cast<MenuRatio>(std::get<uint32_t>(cur_value));
        });

    // RT64's render to RAM, which draws every frame a second time at the N64's resolution and copies it into RDRAM,
    // where the console's RDP leaves it. Nothing in the game was seen reading a frame on the CPU: with page guards on
    // the framebuffers and the Z-buffer through boot, the attract races, the menus, a race with pause, Rewind and
    // Quit, two-player split screen and a Ghost Race to its results, the game's only touches were the ROM inflate's
    // staging buffer in a framebuffer it isn't showing, and no screen samples a framebuffer as a texture. Without the
    // copy a race list on an RTX 2070 at 960x720 took 3.9 ms instead of 5.0 and the GPU drew 41-49 W instead of
    // 60-62, and in a browser a list waited ~6 ms less for the GPU process (13-14 ms a list with it, 5-6 without).
    // Still, a game that never reads its frames can behave differently without them (a mod, or a path nobody
    // guarded), so the copy is an option. The render context reads it when it is made, at startup after this
    // loads, so a change takes effect on the next start. Its default is set here first: a first start has no
    // graphics.json, and then loading calls no callback, which would leave RT64's own default (on).
    recompui::renderer::set_render_to_ram(false);
    graphics.add_enum_option(
        copy_frames_to_ram_option,
        "Copy Frames to RAM",
        "Whether every frame is also copied back into the console's memory, as the N64 leaves it there for the game "
        "to read. The copy is a second draw of each frame at 320x240, and in a browser it holds each frame back by "
        "about 6 ms."
        "<br/>"
        "<b>Off:</b> no copy. The game isn't known to read its frames back. "
        "<b>Original:</b> every frame is copied, as on the console. Try it if something looks or behaves wrong."
        "<br/>"
        "Takes effect the next time the game is started.",
        copy_frames_to_ram_options,
        CopyFramesToRam::Off
    );
    // Called on load too, before the render context is made.
    graphics.add_option_change_callback(copy_frames_to_ram_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            recompui::renderer::set_render_to_ram(
                static_cast<CopyFramesToRam>(std::get<uint32_t>(cur_value)) == CopyFramesToRam::Original);
        });

    recompui::config::create_controls_tab();

    recomp::config::Config& sound = recompui::config::create_sound_tab();
    sound.add_enum_option(
        musyx_mix_option,
        "MusyX Mixing",
        "<b>High Quality</b> mixes the game's music and sound effects at 44.1 kHz with a sharper interpolator, so "
        "sounds played above their recorded pitch keep their treble and loud passages don't clip. "
        "<b>Original</b> plays the game's own 22 kHz mix.",
        musyx_mix_options,
        MusyxMix::HighQuality
    );
    // Called on load too.
    sound.add_option_change_callback(musyx_mix_option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2049::set_musyx_hires(static_cast<MusyxMix>(std::get<uint32_t>(cur_value)) == MusyxMix::HighQuality);
        });

    recompui::config::create_mods_tab();

    recompui::config::finalize();
}
