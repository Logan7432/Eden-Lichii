// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "video_core/frame_generation.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace VideoCore::FrameGeneration {
namespace {

std::mutex state_mutex;
State current_state = State::Off;
std::string current_message;
std::atomic<u64> presented_frames{0};
std::atomic<u64> real_frames{0};

std::mutex rate_mutex;
std::chrono::steady_clock::time_point rate_window_start{};
u64 rate_window_presented = 0;
double output_rate = 0.0;

std::filesystem::path FromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool IsFile(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

std::optional<std::filesystem::path> InInstall(const std::filesystem::path& folder) {
    for (const auto& candidate : {folder / "Lossless.dll", folder / "Lossless Scaling" / "Lossless.dll"}) {
        if (IsFile(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> InChildren(const std::filesystem::path& parent) {
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec)) {
        return std::nullopt;
    }
    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const auto name = entry.path().filename().u8string();
        if (name.rfind(u8"Lossless Scaling", 0) == 0 || name.rfind(u8"LosslessScaling", 0) == 0) {
            if (auto dll = InInstall(entry.path())) {
                return dll;
            }
        }
    }
    return std::nullopt;
}

#ifdef _WIN32
std::filesystem::path SteamPath() {
    std::array<wchar_t, 1024> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, buffer.data(), &size) == ERROR_SUCCESS) {
        return std::filesystem::path(buffer.data());
    }
    return {};
}

std::vector<std::filesystem::path> SteamLibraries(const std::filesystem::path& steam) {
    std::vector<std::filesystem::path> libraries{steam};
    std::ifstream file(steam / "steamapps" / "libraryfolders.vdf");
    std::string line;
    while (std::getline(file, line)) {
        const auto key = line.find("\"path\"");
        if (key == std::string::npos) {
            continue;
        }
        const auto open = line.find('"', key + 6);
        const auto close = open == std::string::npos ? open : line.find('"', open + 1);
        if (open == std::string::npos || close == std::string::npos) {
            continue;
        }
        std::string value = line.substr(open + 1, close - open - 1);
        std::string unescaped;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') {
                ++i;
            }
            unescaped.push_back(value[i]);
        }
        libraries.emplace_back(FromUtf8(unescaped));
    }
    return libraries;
}
#endif

} // Anonymous namespace

bool Enabled() {
    return Settings::values.frame_generation.GetValue();
}

u32 Multiplier() {
    return static_cast<u32>(Settings::values.frame_generation_multiplier.GetValue()) + 2;
}

void SetState(State state, std::string message) {
    std::scoped_lock lock{state_mutex};
    current_state = state;
    current_message = std::move(message);
}

State GetState() {
    std::scoped_lock lock{state_mutex};
    return current_state;
}

double OutputFps() {
    if (GetState() != State::Active) {
        return 0.0;
    }
    std::scoped_lock lock{rate_mutex};
    const auto now = std::chrono::steady_clock::now();
    const u64 presented = presented_frames.load();
    if (rate_window_start == std::chrono::steady_clock::time_point{}) {
        rate_window_start = now;
        rate_window_presented = presented;
        return output_rate;
    }
    const double elapsed = std::chrono::duration<double>(now - rate_window_start).count();
    if (elapsed >= 0.45) {
        output_rate = static_cast<double>(presented - rate_window_presented) / elapsed;
        rate_window_start = now;
        rate_window_presented = presented;
    }
    return output_rate;
}

void CountPresented(u32 real, u32 generated) {
    real_frames += real;
    presented_frames += real + generated;
}

Snapshot GetSnapshot() {
    Snapshot snapshot;
    {
        std::scoped_lock lock{state_mutex};
        snapshot.state = current_state;
        snapshot.message = current_message;
    }
    snapshot.presented_frames = presented_frames.load();
    snapshot.real_frames = real_frames.load();
    snapshot.time = std::chrono::steady_clock::now();
    return snapshot;
}

std::optional<std::filesystem::path> FindLosslessDll() {
    return FindLosslessDll(Settings::values.lossless_dll_path.GetValue());
}

std::optional<std::filesystem::path> FindLosslessDll(const std::string& configured) {
    if (!configured.empty()) {
        const auto path = FromUtf8(configured);
        if (IsFile(path)) {
            return path;
        }
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            return InInstall(path);
        }
        return std::nullopt;
    }

#ifdef _WIN32
    std::filesystem::path folder = Common::FS::GetExeDirectory();
#else
    std::error_code cwd_ec;
    std::filesystem::path folder = std::filesystem::current_path(cwd_ec);
#endif
    for (int depth = 0; depth < 4 && !folder.empty(); ++depth) {
        if (auto dll = depth == 0 ? InInstall(folder) : std::nullopt) {
            return dll;
        }
        if (auto dll = InChildren(folder)) {
            return dll;
        }
        if (!folder.has_parent_path() || folder.parent_path() == folder) {
            break;
        }
        folder = folder.parent_path();
    }

#ifdef _WIN32
    if (const auto steam = SteamPath(); !steam.empty()) {
        for (const auto& library : SteamLibraries(steam)) {
            if (auto dll = InInstall(library / "steamapps" / "common" / "Lossless Scaling")) {
                return dll;
            }
        }
    }
    for (const char* root : {"C:\\Program Files (x86)\\Steam", "C:\\Program Files\\Steam"}) {
        if (auto dll = InInstall(std::filesystem::path(root) / "steamapps" / "common" /
                                 "Lossless Scaling")) {
            return dll;
        }
    }
#endif
    return std::nullopt;
}

} // namespace VideoCore::FrameGeneration
