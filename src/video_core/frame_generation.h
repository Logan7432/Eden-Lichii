// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#include "common/common_types.h"

namespace VideoCore::FrameGeneration {

enum class State : u8 {
    Off,
    Starting,
    Active,
    MissingDll,
    Unsupported,
    Failed,
};

struct Snapshot {
    State state = State::Off;
    std::string message;
    u64 presented_frames = 0;
    u64 real_frames = 0;
    std::chrono::steady_clock::time_point time;
};

bool Enabled();
u32 Multiplier();

void SetState(State state, std::string message = {});
State GetState();
void CountPresented(u32 real, u32 generated);
Snapshot GetSnapshot();
double OutputFps();

std::optional<std::filesystem::path> FindLosslessDll();
std::optional<std::filesystem::path> FindLosslessDll(const std::string& configured);

} // namespace VideoCore::FrameGeneration
