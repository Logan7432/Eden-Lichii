// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <QTimer>
#include <QWidget>

#include "common/settings.h"
#include "common/settings_input.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "lichii/util/controller_navigation.h"

namespace {

constexpr int kRepeatDelayMs = 380;
constexpr int kRepeatIntervalMs = 90;
constexpr int kFastRepeatIntervalMs = 55;
constexpr int kRepeatsBeforeFast = 6;

bool IsRepeatable(ControllerNavigation::Button button) {
    using Button = ControllerNavigation::Button;
    switch (button) {
    case Button::Up:
    case Button::Down:
    case Button::Left:
    case Button::Right:
    case Button::ZL:
    case Button::ZR:
    case Button::RStickUp:
    case Button::RStickDown:
        return true;
    default:
        return false;
    }
}

}

ControllerNavigation::ControllerNavigation(Core::HID::HIDCore& hid_core, QWidget* parent)
    : QObject(parent) {
    qRegisterMetaType<ControllerNavigation::Button>();

    repeat_timer = new QTimer(this);
    repeat_timer->setSingleShot(true);
    connect(repeat_timer, &QTimer::timeout, this, &ControllerNavigation::OnRepeat);

    player1_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Player1);
    handheld_controller = hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    Core::HID::ControllerUpdateCallback engine_callback{
        .on_change = [this](Core::HID::ControllerTriggerType type) { ControllerUpdateEvent(type); },
        .is_npad_service = false,
    };
    player1_callback_key = player1_controller->SetCallback(engine_callback);
    handheld_callback_key = handheld_controller->SetCallback(engine_callback);
    is_controller_set = true;
}

ControllerNavigation::~ControllerNavigation() {
    UnloadController();
}

void ControllerNavigation::UnloadController() {
    if (is_controller_set) {
        player1_controller->DeleteCallback(player1_callback_key);
        handheld_controller->DeleteCallback(handheld_callback_key);
        is_controller_set = false;
    }
    repeat_timer->stop();
}

void ControllerNavigation::ControllerUpdateEvent(Core::HID::ControllerTriggerType type) {
    if (type != Core::HID::ControllerTriggerType::Button &&
        type != Core::HID::ControllerTriggerType::Stick) {
        return;
    }
    if (!Settings::values.controller_navigation) {
        return;
    }

    u32 state;
    {
        std::scoped_lock lock{mutex};
        state = ReadButtonState();
        if (state == posted_state) {
            return;
        }
        posted_state = state;
    }
    QMetaObject::invokeMethod(this, [this, state] { ProcessState(state); }, Qt::QueuedConnection);
}

u32 ControllerNavigation::ReadButtonState() const {
    using Settings::NativeButton::Values;
    const auto controller_type = player1_controller->GetNpadStyleIndex();
    const auto& player1_buttons = player1_controller->GetButtonsValues();
    const auto& handheld_buttons = handheld_controller->GetButtonsValues();
    const auto& player1_sticks = player1_controller->GetSticksValues();
    const auto& handheld_sticks = handheld_controller->GetSticksValues();

    const auto pressed = [&](Values button) {
        return player1_buttons[button].value || handheld_buttons[button].value;
    };
    const auto stick = [&](Settings::NativeAnalog::Values index) {
        return Common::Input::StickStatus{
            .left = player1_sticks[index].left || handheld_sticks[index].left,
            .right = player1_sticks[index].right || handheld_sticks[index].right,
            .up = player1_sticks[index].up || handheld_sticks[index].up,
            .down = player1_sticks[index].down || handheld_sticks[index].down,
        };
    };

    u32 state = 0;
    const auto set = [&state](Button button, bool value) {
        if (value) {
            state |= Bit(button);
        }
    };

    switch (controller_type) {
    case Core::HID::NpadStyleIndex::Fullkey:
    case Core::HID::NpadStyleIndex::JoyconDual:
    case Core::HID::NpadStyleIndex::Handheld:
    case Core::HID::NpadStyleIndex::GameCube: {
        const auto left_stick = stick(Settings::NativeAnalog::LStick);
        const auto right_stick = stick(Settings::NativeAnalog::RStick);
        set(Button::A, pressed(Settings::NativeButton::A));
        set(Button::B, pressed(Settings::NativeButton::B));
        set(Button::X, pressed(Settings::NativeButton::X));
        set(Button::Y, pressed(Settings::NativeButton::Y));
        set(Button::L, pressed(Settings::NativeButton::L));
        set(Button::R, pressed(Settings::NativeButton::R));
        set(Button::ZL, pressed(Settings::NativeButton::ZL));
        set(Button::ZR, pressed(Settings::NativeButton::ZR));
        set(Button::Plus, pressed(Settings::NativeButton::Plus));
        set(Button::Minus, pressed(Settings::NativeButton::Minus));
        set(Button::Up, pressed(Settings::NativeButton::DUp) || left_stick.up);
        set(Button::Down, pressed(Settings::NativeButton::DDown) || left_stick.down);
        set(Button::Left, pressed(Settings::NativeButton::DLeft) || left_stick.left);
        set(Button::Right, pressed(Settings::NativeButton::DRight) || left_stick.right);
        set(Button::RStickUp, right_stick.up);
        set(Button::RStickDown, right_stick.down);
        break;
    }
    case Core::HID::NpadStyleIndex::JoyconLeft: {
        const auto left_stick = stick(Settings::NativeAnalog::LStick);
        set(Button::A, pressed(Settings::NativeButton::DDown));
        set(Button::B, pressed(Settings::NativeButton::DLeft));
        set(Button::X, pressed(Settings::NativeButton::DRight));
        set(Button::Y, pressed(Settings::NativeButton::DUp));
        set(Button::L, pressed(Settings::NativeButton::SLLeft));
        set(Button::R, pressed(Settings::NativeButton::SRLeft));
        set(Button::Plus, pressed(Settings::NativeButton::Minus));
        set(Button::Down, left_stick.left);
        set(Button::Left, left_stick.up);
        set(Button::Right, left_stick.down);
        set(Button::Up, left_stick.right);
        break;
    }
    case Core::HID::NpadStyleIndex::JoyconRight: {
        const auto right_stick = stick(Settings::NativeAnalog::RStick);
        set(Button::A, pressed(Settings::NativeButton::X));
        set(Button::B, pressed(Settings::NativeButton::A));
        set(Button::X, pressed(Settings::NativeButton::Y));
        set(Button::Y, pressed(Settings::NativeButton::B));
        set(Button::L, pressed(Settings::NativeButton::SLRight));
        set(Button::R, pressed(Settings::NativeButton::SRRight));
        set(Button::Plus, pressed(Settings::NativeButton::Plus));
        set(Button::Down, right_stick.right);
        set(Button::Left, right_stick.down);
        set(Button::Right, right_stick.up);
        set(Button::Up, right_stick.left);
        break;
    }
    default:
        break;
    }
    return state;
}

void ControllerNavigation::ProcessState(u32 state) {
    const u32 newly_pressed = state & ~current_state;
    current_state = state;

    if (repeat_button != Button::Count && !(state & Bit(repeat_button))) {
        repeat_timer->stop();
        repeat_button = Button::Count;
    }

    for (u32 i = 0; i < static_cast<u32>(Button::Count); ++i) {
        const auto button = static_cast<Button>(i);
        if (!(newly_pressed & Bit(button))) {
            continue;
        }
        Fire(button);
        if (IsRepeatable(button)) {
            repeat_button = button;
            repeat_count = 0;
            repeat_timer->start(kRepeatDelayMs);
        }
    }
}

void ControllerNavigation::OnRepeat() {
    if (repeat_button == Button::Count || !(current_state & Bit(repeat_button))) {
        repeat_button = Button::Count;
        return;
    }
    Fire(repeat_button);
    ++repeat_count;
    repeat_timer->start(repeat_count < kRepeatsBeforeFast ? kRepeatIntervalMs
                                                          : kFastRepeatIntervalMs);
}

void ControllerNavigation::Fire(Button button) {
    emit ButtonPressed(button);

    switch (button) {
    case Button::A:
        emit TriggerKeyboardEvent(Qt::Key_Enter);
        break;
    case Button::B:
        emit TriggerKeyboardEvent(Qt::Key_Escape);
        break;
    case Button::Up:
        emit TriggerKeyboardEvent(Qt::Key_Up);
        break;
    case Button::Down:
        emit TriggerKeyboardEvent(Qt::Key_Down);
        break;
    case Button::Left:
        emit TriggerKeyboardEvent(Qt::Key_Left);
        break;
    case Button::Right:
        emit TriggerKeyboardEvent(Qt::Key_Right);
        break;
    default:
        break;
    }
}
