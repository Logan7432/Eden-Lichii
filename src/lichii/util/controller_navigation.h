// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

#include <QKeyEvent>
#include <QObject>

#include "common/input.h"
#include "common/settings_input.h"

class QTimer;

namespace Core::HID {
using ButtonValues = std::array<Common::Input::ButtonStatus, Settings::NativeButton::NumButtons>;
using SticksValues = std::array<Common::Input::StickStatus, Settings::NativeAnalog::NumAnalogs>;
enum class ControllerTriggerType;
class EmulatedController;
class HIDCore;
} // namespace Core::HID

class ControllerNavigation : public QObject {
    Q_OBJECT

public:
    enum class Button : u8 {
        A,
        B,
        X,
        Y,
        L,
        R,
        ZL,
        ZR,
        Plus,
        Minus,
        Up,
        Down,
        Left,
        Right,
        RStickUp,
        RStickDown,
        Count,
    };

    explicit ControllerNavigation(Core::HID::HIDCore& hid_core, QWidget* parent = nullptr);
    ~ControllerNavigation() override;

    /// Disables events from the emulated controller
    void UnloadController();

signals:
    void TriggerKeyboardEvent(Qt::Key key);
    void ButtonPressed(ControllerNavigation::Button button);
    void TriggerContextMenu();

private:
    void ControllerUpdateEvent(Core::HID::ControllerTriggerType type);
    u32 ReadButtonState() const;
    void ProcessState(u32 state);
    void Fire(Button button);
    void OnRepeat();

    static constexpr u32 Bit(Button button) {
        return 1U << static_cast<u32>(button);
    }

    u32 posted_state{};
    u32 current_state{};
    Button repeat_button{Button::Count};
    int repeat_count{};
    QTimer* repeat_timer = nullptr;

    int player1_callback_key{};
    int handheld_callback_key{};
    bool is_controller_set{};
    mutable std::mutex mutex;
    Core::HID::EmulatedController* player1_controller;
    Core::HID::EmulatedController* handheld_controller;
};

Q_DECLARE_METATYPE(ControllerNavigation::Button);
