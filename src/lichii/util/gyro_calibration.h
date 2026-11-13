// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/settings_input.h"

class QWidget;

namespace Core::HID {
class EmulatedController;
}

namespace GyroCalibration {

void Run(QWidget* parent, Core::HID::EmulatedController* controller,
         const std::array<bool, Settings::NativeMotion::NumMotions>& motion_slots);

void RunForActivePlayer(QWidget* parent);

}
