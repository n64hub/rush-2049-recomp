// San Francisco Rush 2049: Recompiled - entry point.
//
// Structure follows BanjoRecomp's src/main/main.cpp (GPL-3.0), trimmed to what this project uses so far: RecompFrontend's
// launcher with the project's own artwork (assets/launcher/, made by tools/launcher_art.py). Mod support (patches
// library, exports) is in register_patches.cpp and mod_exports.cpp.

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef __EMSCRIPTEN__
#include "audio_output.h"
#else
#include "nfd.h" // a browser has no native file dialogs; RecompFrontend asks the page instead
#endif

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
#undef None
#undef Status
#undef LockMask
#undef ControlMask
#undef Success
#undef Always
#endif

#include "recompui/recompui.h"
#include "recompui/program_config.h"
#include "recompui/renderer.h"
#include "recompui/config.h"
#include "util/file.h"
#include "recompinput/input_events.h"
#include "recompinput/recompinput.h"
#include "recompinput/profiles.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/helpers.hpp"
#include "librecomp/rsp.hpp"

#include "common/rt64_gpu_timings.h"
#include "rush2049_game.h"
#include "crash_handler.h"
#include "audio_limiter.h"
#include "audio_resampler.h"
#include "debug_probe.h"
#include "musyx_hle.h"
#include "rom_finder.h"

#if defined(__linux__)
#include "rush2049_icon_bmp.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>
#include "SDL_syswm.h"
#endif

const std::string version_string = "0.1.36";

template<typename... Ts>
void exit_error(const char* str, Ts ...args) {
    ((void)fprintf(stderr, str, args), ...);
    assert(false);
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    // Emscripten's SDL2 is built without haptics, and asking for them there fails the whole call, gamepads with it.
    // Nothing uses them: rumble goes through SDL_JoystickRumble.
#ifdef __EMSCRIPTEN__
    const Uint32 sdl_subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK;
#else
    const Uint32 sdl_subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC;
#endif
    if (SDL_Init(sdl_subsystems) < 0) {
        exit_error("Failed to initialize SDL2: %s\n", SDL_GetError());
    }

    fprintf(stdout, "SDL Video Driver: %s\n", SDL_GetCurrentVideoDriver());
    return {};
}

// --probes: log each change of player 1's buttons as the game reads them.
static bool log_input = false;
static int fake_players = 0;

// --input <file>: scripted port 1 input for unattended test runs, and for every port --players adds. Each line is "<seconds> <buttons hex> [x y]",
// seconds counted from the first input read, and the last line whose time has passed is held. Lines starting with #
// are comments. Bit 0x10000, which is no N64 button, holds Rewind.
struct ScriptedInput {
    double at;
    uint32_t buttons;
    float x, y;
};
static std::vector<ScriptedInput> input_script;

static void load_input_script(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        fprintf(stderr, "--input: can't open %s\n", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != nullptr) {
        ScriptedInput in{};
        if (line[0] != '#' && sscanf(line, "%lf %x %f %f", &in.at, &in.buttons, &in.x, &in.y) >= 2) {
            input_script.push_back(in);
        }
    }
    fclose(f);
}

static const ScriptedInput* scripted_input() {
    if (input_script.empty()) {
        return nullptr;
    }
    static const auto start = std::chrono::steady_clock::now();
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const ScriptedInput* current = nullptr;
    for (const ScriptedInput& in : input_script) {
        if (in.at <= now) {
            current = &in;
        }
    }
    return current;
}

bool rush2049::scripted_rewind() {
    const ScriptedInput* in = scripted_input();
    return in != nullptr && (in->buttons & 0x10000) != 0;
}

// Whether a player plays on this port. Port 1 always: the keyboard drives player 1 before any controller has joined,
// and --input drives it. In multiplayer mode (the Controllers option's Per Player) also the ports of the players who
// have joined (src/config.cpp). --players N: ports 1..N, so split screen can be tested with one pad (the extra ports
// get port 1's input).
static bool port_has_player(int port) {
    bool assigned = port == 0 || (!recompinput::players::is_single_player_mode() && recompinput::players::get_player_is_assigned(port));
    return assigned || port < fake_players;
}

static bool get_n64_input_logged(int player_index, uint16_t* buttons_out, float* x_out, float* y_out) {
    // A port with nobody on it still has a controller (see get_connected_device_info), left alone.
    if (!port_has_player(player_index)) {
        *buttons_out = 0;
        *x_out = 0.0f;
        *y_out = 0.0f;
        return true;
    }
    bool ret = recompinput::profiles::get_n64_input(player_index, buttons_out, x_out, y_out);
    if (const ScriptedInput* in = scripted_input(); in != nullptr && (player_index == 0 || player_index < fake_players)) {
        *buttons_out = (uint16_t)in->buttons;
        *x_out = in->x;
        *y_out = in->y;
        ret = true;
    }
    if (log_input && player_index == 0) {
        rush2049::probe_input_reads++;
        static uint16_t last_buttons = 0;
        if (*buttons_out != last_buttons) {
            last_buttons = *buttons_out;
            fprintf(stderr, "[input] p1 buttons %04X\n", last_buttons);
        }
    }
    return ret;
}

static void set_rumble_logged(int controller_num, bool rumble) {
    static bool logged[4] = {};
    if (rumble && controller_num >= 0 && controller_num < 4 && !logged[controller_num]) {
        logged[controller_num] = true;
        fprintf(stderr, "[rumble] first motor start on port %d\n", controller_num + 1);
    }
    recompinput::set_rumble(controller_num, rumble);
}

ultramodern::input::connected_device_info_t get_connected_device_info(int) {
    // All four ports always have a controller with the accessory picked in the config plugged in, so every port's
    // Controller Pak (src/controller_pak.cpp) can be loaded from and saved to. A port nobody plays on reads as a
    // controller left alone (get_n64_input_logged). osPfsInitPak fails for a Rumble Pak, and the runtime's
    // osMotorInit always succeeds, so reporting RumblePak here for every Controller Accessory is harmless (with Both,
    // osMotorInit is called on the Controller Pak's OSPfs: src/controller_pak.cpp).
    return ultramodern::input::connected_device_info_t{
        .connected_device = ultramodern::input::Device::Controller,
        .connected_pak = ultramodern::input::Pak::RumblePak,
    };
}

SDL_Window* window;
// --window <w>x<h> sets the size the window opens at, to measure settings at a given size.
static int window_width = 1600;
static int window_height = 1200;

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    uint32_t flags = SDL_WINDOW_RESIZABLE;

#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif defined(RT64_SDL_WINDOW_VULKAN)
    flags |= SDL_WINDOW_VULKAN;
#endif

    window = SDL_CreateWindow(rush2049::program_name.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_width, window_height, flags);
    if (window == nullptr) {
        exit_error("Failed to create window: %s\n", SDL_GetError());
    }

#if defined(__linux__)
    // On Windows SDL gives the window the executable's icon (icon/rush2049.rc), in every size it holds. Elsewhere the
    // window has none until it is given one: icon/rush2049.bmp, built in (CMakeLists.txt).
    if (SDL_Surface* icon = SDL_LoadBMP_RW(SDL_RWFromConstMem(rush2049_icon_bmp, (int)rush2049_icon_bmp_size), 1)) {
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    }
#endif

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);

#if defined(_WIN32)
    return ultramodern::renderer::WindowHandle{ wmInfo.info.win.window, GetCurrentThreadId() };
#elif defined(__linux__) || defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    // In a browser the window is the page's canvas, sized by the page (SDL follows its CSS size).
    return ultramodern::renderer::WindowHandle{ window };
#elif defined(__APPLE__)
    SDL_MetalView view = SDL_Metal_CreateView(window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.cocoa.window, SDL_Metal_GetLayer(view) };
#else
    static_assert(false && "Unimplemented");
#endif
}

void update_gfx(void*) {
    recompinput::handle_events();
}

// Audio output: the game's samples (s16 stereo, channels swapped by the runtime's address xor) are converted to
// float and resampled to the device rate (rush2049::Resampler). With the high-quality MusyX mix on, each buffer is
// first swapped for the same audio rendered at twice the game's rate. At the device rate it is halved and then limited
// (rush2049::Limiter), since that mix isn't clamped to 16 bits and its peaks reach about twice the game's full scale.
//
// The game is fed by the runtime's VI clock and the device plays on its own, so what SDL holds drifts, and swings by
// tens of ms as the game queues its 8.7 ms buffers in bursts. Rather than throwing samples away when it runs long,
// the resampler plays up to 1% faster or slower (17 cents, too little to hear as pitch) to keep it near
// audio_queue_target_ms: a long queue drains and a short one refills without a seam.
//
// The game queues its buffers on the VI clock (an AI event per buffer played, N64ModernRuntime-0001), not by how much
// the device holds, so the queue stays as deep as it started. Its first buffer reaches an empty device, so the device
// is filled with silence up to audio_queue_target_ms ahead of it, and again whenever it has run dry: started a buffer
// deep, the queue took seconds to reach the target at the resampler's pace, and on a page the game's first moments
// left the device 3-17 ms without a buffer.
//
// On a page the device is N64Recomp-browser's AudioWorklet output (audio_output.h) instead of SDL's queue, which
// Emscripten's SDL2 plays from the page's thread: the music broke up whenever WebGPU kept that thread busy.

static rush2049::Resampler resampler;
static uint32_t resampler_input_rate = 0;
static SDL_AudioDeviceID audio_device = 0;
static uint32_t sample_rate = 48000;
static uint32_t output_sample_rate = 48000;
static uint32_t output_channels = 2;
constexpr uint32_t bytes_per_frame = 2 * sizeof(float);
constexpr double audio_queue_target_ms = 40.0;
constexpr double audio_speed_ppm_per_ms = 200.0;  // 1% at 50 ms off target
constexpr double audio_max_speed_ppm = 10000.0;

#ifdef __EMSCRIPTEN__
static size_t device_queued_frames() {
    return n64recomp_browser::audio::queued_frames();
}

static void device_queue(const float* frames, size_t frame_count) {
    n64recomp_browser::audio::queue(frames, frame_count);
}
#else
static size_t device_queued_frames() {
    return SDL_GetQueuedAudioSize(audio_device) / bytes_per_frame;
}

static void device_queue(const float* frames, size_t frame_count) {
    SDL_QueueAudio(audio_device, frames, uint32_t(frame_count * bytes_per_frame));
}
#endif

// The rate the samples reach the converter at.
static uint32_t input_sample_rate() {
    return rush2049::musyx_hires_enabled() ? 2 * sample_rate : sample_rate;
}

void update_audio_converter();

void queue_samples(int16_t* audio_data, size_t sample_count) {
    // The Sound tab can switch the high-quality mix, and with it the input rate, at any time.
    if (input_sample_rate() != resampler_input_rate) {
        update_audio_converter();
    }
    // Left, right, with the game's 16-bit full scale at 1.0.
    static std::vector<float> input;
    if (rush2049::musyx_hires_enabled()) {
        rush2049::musyx_hires_buffer(audio_data, sample_count, input);
        for (float& v : input) {
            v *= 1.0f / 32768.0f;
        }
    }
    else {
        input.resize(sample_count);
        for (size_t i = 0; i < sample_count; i += 2) {
            input[i + 0] = audio_data[i + 1] * (1.0f / 32768.0f);
            input[i + 1] = audio_data[i + 0] * (1.0f / 32768.0f);
        }
    }
    rush2049::probe_audio(input.data(), input.size() / 2);

    // Real time queued on the device, smoothed over about 30 buffers (a quarter of a second).
    uint32_t queued_us = uint32_t(uint64_t(device_queued_frames()) * 1000000 / output_sample_rate);
    static double queued_ms = audio_queue_target_ms;
    queued_ms += (queued_us / 1000.0 - queued_ms) / 32.0;
    double speed_ppm = std::clamp((queued_ms - audio_queue_target_ms) * audio_speed_ppm_per_ms, -audio_max_speed_ppm,
                                  audio_max_speed_ppm);
    resampler.set_speed((int32_t)speed_ppm);
    rush2049::probe_audio_queue(queued_us, speed_ppm > 0.0);

    static std::vector<float> output;
    output.clear();
    resampler.process(input.data(), input.size() / 2, output);
    // Halved, so the game's full scale doesn't clip on the device, and limited, so neither do the high-quality mix's
    // peaks above it.
    float main_volume = static_cast<float>(recompui::config::sound::get_main_volume()) / 100.0f;
    for (float& v : output) {
        v *= 0.5f * main_volume;
    }
    rush2049::output_limiter.process(output.data(), output.size() / 2);

    size_t frames = output.size() / 2;
    rush2049::probe_output(output.data(), frames, output_sample_rate);
    size_t target_frames = size_t(audio_queue_target_ms * output_sample_rate / 1000.0);
    if (queued_us == 0 && frames < target_frames) {
        static std::vector<float> silence;
        silence.assign((target_frames - frames) * 2, 0.0f);
        device_queue(silence.data(), target_frames - frames);
    }
    device_queue(output.data(), frames);
}

size_t get_frames_remaining() {
    constexpr float buffer_offset_frames = 1.0f;
    uint64_t buffered_byte_count = uint64_t(device_queued_frames()) * bytes_per_frame;
    buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;

    uint32_t frames_per_vi = (sample_rate / 60);
    if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
        buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
    }
    else {
        buffered_byte_count = 0;
    }
    return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
}

void update_audio_converter() {
    resampler_input_rate = input_sample_rate();
    resampler.reset(resampler_input_rate, output_sample_rate);
    rush2049::probe_audio_frequency(resampler_input_rate);
}

void set_frequency(uint32_t freq) {
    sample_rate = freq;
    update_audio_converter();
}

bool reset_audio(uint32_t output_freq) {
#ifdef __EMSCRIPTEN__
    output_freq = n64recomp_browser::audio::open(output_channels);
#else
    SDL_AudioSpec spec_desired{
        .freq = (int)output_freq,
        .format = AUDIO_F32,
        .channels = (Uint8)output_channels,
        .silence = 0,
        .samples = 0x100,
        .padding = 0,
        .size = 0,
        .callback = nullptr,
        .userdata = nullptr
    };

    audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
    if (audio_device == 0) {
        std::string audio_error = std::string("No audio device could be found. Please make sure an audio device is available.\nError opening audio device: ") + std::string(SDL_GetError());
        recompui::message_box(audio_error.c_str());
        return false;
    }

    SDL_PauseAudioDevice(audio_device, 0);
#endif
    output_sample_rate = output_freq;
    rush2049::output_limiter.reset(output_sample_rate);
    update_audio_converter();
    return true;
}

// The game's audio runs on MusyX v1's RSP microcode ("N64 RSP mixer V1.1"); see musyx_hle.cpp.
RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return rush2049::get_musyx_ucode();

    default:
        fprintf(stderr, "Unknown task: %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

std::vector<recomp::GameEntry> supported_games = {
    {
        // XXH3 of San Francisco Rush 2049 (USA), SHA-1 3f99351d7bb61656614bdb2aa1a90cfe55d1922c.
        .rom_hash = 0x86A0D6C1C9A3DEA1ULL,
        .internal_name = "Rush 2049", // as in the header at 0x20; the runtime compares it exactly
        .display_name = "San Francisco Rush 2049",
        .game_id = u8"rush2049.n64.us",
        .mod_game_id = "rush2049",
        // Saves go to the Controller Pak, which the runtime doesn't emulate yet.
        .save_type = recomp::SaveType::None,
        .is_enabled = false,
        // Not compressed for the game itself (it inflates its own code), but the runtime needs the inflated code to
        // recompile a function a mod hooks: see build_recomp_rom.
        .decompression_routine = rush2049::build_recomp_rom,
        .has_compressed_code = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
    },
};

// --boot (development): start the game on the launcher's first frame, as its "Start Game" option does, so
// bring-up runs don't need someone at the window. Needs the ROM already stored (src/rom_finder.cpp).
static bool autoboot = false;
static bool show_console = false;

void on_launcher_update(recompui::LauncherMenu*) {
    static bool done = false;
    if (!autoboot || done) {
        return;
    }
    done = true;
    std::u8string game_id = supported_games[0].game_id;
    if (!recomp::is_rom_valid(game_id)) {
        fprintf(stderr, "--boot: no valid stored ROM, staying in the launcher\n");
        return;
    }
    recomp::start_game(game_id, "");
    recompui::hide_all_contexts();
}

void on_launcher_init(recompui::LauncherMenu* menu) {
    // Before the menu reads whether a ROM is stored, so a found one starts on Start Game.
    recomp::GameEntry& game = supported_games[0];
    if (!recomp::is_rom_valid(game.game_id) && rush2049::find_rom(game.game_id, game.internal_name)) {
        recomp::check_all_stored_roms();
    }

    // The project's own artwork (tools/launcher_art.py) instead of RecompFrontend's flat background and the program's
    // name in Header1 text, which barely fits a 4:3 window. The wordmark is sized by the window's height, 27% of it,
    // centred where that title was, a quarter of the way down, so it stays clear of the menu below the middle.
    menu->remove_default_title();
    menu->set_launcher_background_svg("launcher/background.svg");
    recompui::Element* title = recompui::get_current_context().create_element<recompui::Svg>(
        menu->get_background_container(), "launcher/title.svg");
    title->set_position(recompui::Position::Absolute);
    title->set_top(25.0f, recompui::Unit::Percent);
    title->set_left(50.0f, recompui::Unit::Percent);
    title->set_height(27.0f, recompui::Unit::Percent);
    title->set_width_auto();
    title->set_translate_2D(-50.0f, -50.0f, recompui::Unit::Percent);

    auto game_options_menu = menu->init_game_options_menu(
        supported_games[0].game_id,
        supported_games[0].mod_game_id,
        supported_games[0].display_name,
        supported_games[0].thumbnail_bytes
    );
    game_options_menu->add_default_options();
}

// --gpu-profile: from `after` seconds on, collect RT64's GPU time for every frame for `seconds` seconds, then print
// count, rate, mean, median, 95th percentile and max for the rendered frames and for the present pass, the mean time
// of each part of both (RT64::GPUTimings::RenderPart, PresentPart) with framebuffers and draw calls a frame, and the
// share of the window the GPU spent on both.
//
// The readings are wall time and a desktop GPU's clock wanders (an RTX 2070's 630-1620 MHz under light load), so
// compare in cycles (ms x the clock nvidia-smi reports over the same run) and interleave runs. Check the host too: with
// its CPU busy the game drops below 60 fps and the attract race reaches other scenes, so the draw count moves. Separate
// runs at 240p still differ by +-15% in cycles, so for a change worth less than that, switch it on and off every few
// frames inside one run and time the two halves apart. Write down what else was running beside a number.
//
// Where a frame goes on an RTX 2070, attract race, 240p (Resolution Original) in a 1920x1080 window, thousands of
// cycles (Auto is the 1080p column), measured with render to RAM still on (its second pass is in render):
//
//   part                         240p   1080p
//   render: draws                 116     754   ~500 calls; ~90k of it stays with no pixels
//           vertex compute         27      31
//           uploads                25      35   ~36 at 240p without rt64-0004
//           barriers, resolves     13      21
//           everything else         5       6
//   present: VI pass               71      95   sized by the window: 37 in 1280x720
//            resolve (MSAA 2x)     20      94   3 with MSAA off
//            UI                     6       6
//
// So what doesn't scale with resolution is about 90k of the draws part (not the number of calls: docs/ruled-out.md),
// the VI pass (it scales with the window instead) and about 50k of compute and uploads.
static void start_gpu_profiler(int after_seconds, int seconds) {
    std::thread([after_seconds, seconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(after_seconds));
        RT64::GPUTimings& timings = RT64::gpuTimings;
        {
            std::scoped_lock lock(timings.mutex);
            timings.clear();
            timings.enabled = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::vector<double> render, present;
        double render_parts[RT64::GPUTimings::RenderPartCount], present_parts[RT64::GPUTimings::PresentPartCount];
        uint64_t framebuffers, draw_calls;
        {
            std::scoped_lock lock(timings.mutex);
            timings.enabled = false;
            render.swap(timings.render);
            present.swap(timings.present);
            std::copy(std::begin(timings.renderParts), std::end(timings.renderParts), render_parts);
            std::copy(std::begin(timings.presentParts), std::end(timings.presentParts), present_parts);
            framebuffers = timings.framebuffers;
            draw_calls = timings.drawCalls;
        }
        double busy_ms = 0.0;
        auto report = [&](const char* name, std::vector<double>& ms) {
            if (ms.empty()) {
                printf("[gpuprofile] %s: no frames\n", name);
                return;
            }
            std::sort(ms.begin(), ms.end());
            double sum = 0.0;
            for (double m : ms) {
                sum += m;
            }
            busy_ms += sum;
            printf("[gpuprofile] %s: %zu frames (%.1f/s), mean %.3f ms, median %.3f, p95 %.3f, max %.3f\n", name, ms.size(),
                ms.size() / double(seconds), sum / ms.size(), ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back());
        };
        report("render", render);
        if (!render.empty()) {
            const double n = double(render.size());
            printf("[gpuprofile] render parts, mean ms: uploads %.3f, vertices %.3f, setup %.3f, framebuffer operations %.3f,"
                " draws %.3f, resolves %.3f; %.1f framebuffers and %.0f draw calls a frame\n",
                render_parts[RT64::GPUTimings::RenderUploads] / n, render_parts[RT64::GPUTimings::RenderVertices] / n,
                render_parts[RT64::GPUTimings::RenderSetup] / n,
                render_parts[RT64::GPUTimings::RenderFbOperations] / n, render_parts[RT64::GPUTimings::RenderDraws] / n,
                render_parts[RT64::GPUTimings::RenderResolves] / n, framebuffers / n, draw_calls / n);
        }
        report("present", present);
        if (!present.empty()) {
            const double n = double(present.size());
            printf("[gpuprofile] present parts, mean ms: resolve %.3f, VI %.3f, UI %.3f\n",
                present_parts[RT64::GPUTimings::PresentResolve] / n, present_parts[RT64::GPUTimings::PresentVI] / n,
                present_parts[RT64::GPUTimings::PresentUI] / n);
        }
        printf("[gpuprofile] GPU busy %.1f%% of %d s\n", busy_ms / (seconds * 10.0), seconds);
        fflush(stdout);
    }).detach();
}

int main(int argc, char** argv) {
    rush2049::install_crash_handler();

    recomp::Version project_version{};
    if (!recomp::Version::from_string(version_string, project_version)) {
        ultramodern::error_handling::message_box(("Invalid version string: " + version_string).c_str());
        return EXIT_FAILURE;
    }

#ifdef _WIN32
    timeBeginPeriod(1);
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    // RecompFrontend finds assets/ (and portable.txt) in the working directory, which a desktop launcher or a shell
    // elsewhere doesn't set to the release's folder: when assets/ isn't there, work from the program's own folder.
    // A dev build run from the repo's root keeps the root, which is where assets/ is.
    {
        std::error_code ec;
        if (!std::filesystem::is_directory("assets", ec)) {
            std::filesystem::path exe_dir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
            if (!ec && std::filesystem::is_directory(exe_dir / "assets", ec)) {
                std::filesystem::current_path(exe_dir, ec);
            }
        }
    }
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--boot") == 0) {
            autoboot = true;
        }
        if (strcmp(argv[i], "--show-console") == 0) {
            show_console = true;
        }
        if (strcmp(argv[i], "--stacks") == 0) {
            rush2049::start_stack_sampler(5, true);
        }
        if (strcmp(argv[i], "--probes") == 0) {
            rush2049::start_stack_sampler(5, false);
            log_input = true;
        }
        if (strcmp(argv[i], "--profile") == 0 && i + 2 < argc) {
            int after = atoi(argv[++i]);
            rush2049::start_profiler(after, atoi(argv[++i]));
        }
        if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &window_width, &window_height);
        }
        if (strcmp(argv[i], "--gpu-profile") == 0 && i + 2 < argc) {
            int after = atoi(argv[++i]);
            start_gpu_profiler(after, atoi(argv[++i]));
        }
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            load_input_script(argv[++i]);
        }
        if (strcmp(argv[i], "--players") == 0 && i + 1 < argc) {
            fake_players = atoi(argv[++i]);
        }
        if (strcmp(argv[i], "--hwwatch") == 0 && i + 1 < argc) {
            rush2049::start_hw_watch((uint32_t)strtoul(argv[++i], nullptr, 16));
        }
        if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc) {
            rush2049::start_byte_watch((uint32_t)strtoul(argv[++i], nullptr, 16));
        }
        if (strcmp(argv[i], "--dump-ram") == 0 && i + 2 < argc) {
            const char* prefix = argv[++i];
            rush2049::start_ram_dump(prefix, argv[++i]);
        }
        if (strcmp(argv[i], "--dump-dl") == 0 && i + 2 < argc) {
            const char* path = argv[++i];
            rush2049::start_dl_dump(path, atof(argv[++i]));
        }
        if (strcmp(argv[i], "--dump-audio") == 0 && i + 1 < argc) {
            rush2049::start_audio_dump(argv[++i]);
        }
        if (strcmp(argv[i], "--dump-output") == 0 && i + 1 < argc) {
            rush2049::start_output_dump(argv[++i]);
        }
        if (strcmp(argv[i], "--musyx") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "lle") == 0) {
                rush2049::set_musyx_mode(rush2049::MusyxMode::Lle);
            }
            else if (strcmp(argv[i], "compare") == 0) {
                rush2049::set_musyx_mode(rush2049::MusyxMode::Compare);
            }
            else if (strcmp(argv[i], "native") == 0) {
                rush2049::set_musyx_mode(rush2049::MusyxMode::Native);
            }
            else {
                rush2049::set_musyx_mode(rush2049::MusyxMode::Hle);
            }
        }
    }

#ifdef _WIN32
    if (show_console && GetConsoleWindow() == nullptr) {
        AllocConsole();
        freopen("CONIN$", "r", stdin);
        freopen("CONOUT$", "w", stderr);
        freopen("CONOUT$", "w", stdout);
    }

    SetConsoleOutputCP(CP_UTF8);

    // wasapi; directsound has had sample queueing issues.
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", true);
#endif

#ifndef __EMSCRIPTEN__
    NFD_Init();
    rush2049::install_x_error_handler();
#endif

    recompui::programconfig::set_program_name(rush2049::program_name);
    recompui::programconfig::set_program_id(rush2049::program_id);

#ifndef __EMSCRIPTEN__
    // Started from Explorer, a desktop launcher or Steam nobody sees the log, so it goes to rush2049.log beside the
    // settings and saves. --show-console keeps it in the console, terminal or redirect.
    if (!show_console) {
        rush2049::start_log_file(recompui::file::get_app_folder_path(),
            (std::string{ rush2049::program_name } + " " + version_string).c_str());
    }
#endif

    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!reset_audio(48000)) {
        return EXIT_FAILURE;
    }

    std::u8string controller_db_path = (recompui::file::get_program_path() / "recompcontrollerdb.txt").u8string();
    if (SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char*>(controller_db_path.c_str())) < 0) {
        fprintf(stderr, "Failed to load controller mappings: %s\n", SDL_GetError());
    }

    recompui::register_primary_font("InterVariable.ttf", "Inter Variable");

    recomp::register_config_path(recompui::file::get_app_folder_path());

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    rush2049::register_overlays();
#ifdef RUSH2049_PATCHES
    rush2049::register_patches();
#endif
    rush2049::register_mod_exports();

    // The Controllers option's default, Per Player (src/config.cpp): its change callback sets it when a settings file
    // loads, and with none yet, on the first launch, no callback runs.
    recompinput::players::set_single_player_mode(false);

    rush2049::init_config();

    recompui::register_launcher_init_callback(on_launcher_init);
    recompui::register_launcher_update_callback(on_launcher_update);

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = [](uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
            auto presentation_mode = ultramodern::renderer::PresentationMode::PresentEarly;
            return recompui::renderer::create_render_context(rdram, window_handle, presentation_mode, developer_mode);
        },
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = recompinput::poll_inputs,
        .get_input = get_n64_input_logged,
        .set_rumble = set_rumble_logged,
        .get_connected_device_info = get_connected_device_info,
    };

    ultramodern::events::callbacks_t thread_callbacks{
        .vi_callback = recompinput::update_rumble,
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = [](const char* msg) {
            fprintf(stderr, "[error] %s\n", msg);
            recompui::message_box(msg);
        },
    };

    recomp::start({
        .project_version = project_version,
        .rsp_callbacks = rsp_callbacks,
        .renderer_callbacks = renderer_callbacks,
        .audio_callbacks = audio_callbacks,
        .input_callbacks = input_callbacks,
        .gfx_callbacks = gfx_callbacks,
        .events_callbacks = thread_callbacks,
        .error_handling_callbacks = error_handling_callbacks,
        .threads_callbacks = { .get_game_thread_name = rush2049::game_thread_name },
    });

#ifndef __EMSCRIPTEN__
    NFD_Quit();
#endif

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    return EXIT_SUCCESS;
}
