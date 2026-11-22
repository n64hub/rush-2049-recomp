// Finds the ROM for the launcher, so a first start doesn't depend on knowing where the dump is and what to pick.
//
// Only the folders themselves and one level below them are read (a downloaded ROM is often unzipped into a folder
// of its own), and only files with a ROM extension and a ROM's size, or an archive's extension (.zip, .7z, .gz, .tgz,
// .xz, .txz, .tar) and at most 256 MiB. A ROM is checked by the internal name in its header first, so the runtime's
// hash check (recomp::select_rom, which stores the ROM on a match) only reads files that are this game; an archive goes
// to select_rom as it is, which looks inside it (RECOMP_ROM_ARCHIVES, CMakeLists.txt) and reads only the files in it
// that are ROMs. The ROMs are tried before the archives. Nothing found leaves the launcher on Load ROM, as before.

#include "rom_finder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#include "librecomp/game.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#endif

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t min_rom_size = 1 << 20;
constexpr std::uintmax_t max_rom_size = 64 << 20;
constexpr std::uintmax_t max_archive_size = 256 << 20;

std::string lower_extension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

bool has_rom_extension(const fs::path& path) {
    std::string ext = lower_extension(path);
    return ext == ".z64" || ext == ".n64" || ext == ".v64";
}

bool has_archive_extension(const fs::path& path) {
    std::string ext = lower_extension(path);
    return ext == ".zip" || ext == ".7z" || ext == ".gz" || ext == ".tgz" || ext == ".xz" || ext == ".txz" ||
        ext == ".tar";
}

// Whether the header names this game, in any of the three byte orders.
bool header_matches(const fs::path& path, const std::string& internal_name) {
    std::array<uint8_t, 0x40> header{};
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) {
        return false;
    }
    if (header[0] == 0x37 && header[1] == 0x80) { // .v64: byte-swapped 16-bit words
        for (size_t i = 0; i < header.size(); i += 2) {
            std::swap(header[i], header[i + 1]);
        }
    }
    else if (header[0] == 0x40 && header[1] == 0x12) { // .n64: little-endian 32-bit words
        for (size_t i = 0; i < header.size(); i += 4) {
            std::swap(header[i], header[i + 3]);
            std::swap(header[i + 1], header[i + 2]);
        }
    }
    else if (header[0] != 0x80 || header[1] != 0x37) {
        return false;
    }
    return std::string_view(reinterpret_cast<const char*>(header.data()) + 0x20, internal_name.size()) == internal_name;
}

void add_candidates(const fs::path& dir, int depth, const std::string& internal_name, std::vector<fs::path>& out,
    std::vector<fs::path>& archives) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        std::error_code entry_ec;
        if (entry.is_directory(entry_ec)) {
            if (depth > 0) {
                add_candidates(entry.path(), depth - 1, internal_name, out, archives);
            }
            continue;
        }
        if (!entry.is_regular_file(entry_ec)) {
            continue;
        }
        std::uintmax_t size = entry.file_size(entry_ec);
        if (!entry_ec && size <= max_archive_size && has_archive_extension(entry.path())) {
            archives.push_back(entry.path());
            continue;
        }
        if (entry_ec || !has_rom_extension(entry.path()) || size < min_rom_size || size > max_rom_size) {
            continue;
        }
        if (header_matches(entry.path(), internal_name)) {
            out.push_back(entry.path());
        }
    }
}

// Where to look, with how many levels of subfolders to read under each.
std::vector<std::pair<fs::path, int>> search_folders() {
    std::vector<std::pair<fs::path, int>> folders;
    auto add = [&folders](const fs::path& dir, int depth) {
        std::error_code ec;
        for (const auto& seen : folders) {
            if (fs::equivalent(seen.first, dir, ec)) {
                return;
            }
        }
        folders.emplace_back(dir, depth);
    };
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec) {
        add(cwd, 0);
        add(cwd / "roms", 0);
    }

#ifdef _WIN32
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        fs::path exe_dir = fs::path(exe_path).parent_path();
        add(exe_dir, 0);
        add(exe_dir / "roms", 0);
    }

    for (REFKNOWNFOLDERID id : { FOLDERID_Downloads, FOLDERID_Desktop, FOLDERID_Documents }) {
        PWSTR known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &known))) {
            add(fs::path(known), 1);
        }
        CoTaskMemFree(known);
    }
#else
    if (const char* home = std::getenv("HOME")) {
        for (const char* name : { "Downloads", "Desktop", "Documents" }) {
            add(fs::path(home) / name, 1);
        }
    }
#endif

    return folders;
}

} // namespace

bool rush2049::find_rom(const std::u8string& game_id, const std::string& internal_name) {
    std::vector<fs::path> candidates;
    std::vector<fs::path> archives;
    for (const auto& [dir, depth] : search_folders()) {
        add_candidates(dir, depth, internal_name, candidates, archives);
    }
    candidates.insert(candidates.end(), archives.begin(), archives.end());

    std::u8string id = game_id;
    for (const fs::path& path : candidates) {
        recomp::RomValidationError result = recomp::select_rom(path, id);
        if (result == recomp::RomValidationError::Good) {
            fprintf(stderr, "[rom] found the ROM at %s\n", reinterpret_cast<const char*>(path.u8string().c_str()));
            return true;
        }
        // A ROM: most likely another version of the game (the runtime checks the USA ROM's hash). An archive: most
        // often one of other things.
        fprintf(stderr, "[rom] %s is not and doesn't hold the USA ROM (error %d)\n", reinterpret_cast<const char*>(path.u8string().c_str()), (int)result);
    }
    fprintf(stderr, "[rom] no ROM found in %zu candidate(s); waiting for Load ROM\n", candidates.size());
    return false;
}
