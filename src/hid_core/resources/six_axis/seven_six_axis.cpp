// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include "common/common_types.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "common/settings.h"
#include "hid_core/frontend/emulated_console.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/frontend/emulated_devices.h"
#include "hid_core/hid_core.h"
#include "hid_core/resources/six_axis/seven_six_axis.h"

namespace Service::HID {
SevenSixAxis::SevenSixAxis(Core::System& system_)
    : ControllerBase{system_.HIDCore()}, system{system_} {
    console = hid_core.GetEmulatedConsole();
}

SevenSixAxis::~SevenSixAxis() = default;

void SevenSixAxis::OnInit() {}
void SevenSixAxis::OnRelease() {}

void SevenSixAxis::OnUpdate(const Core::Timing::CoreTiming& core_timing) {
    if (!IsControllerActivated() || transfer_memory == 0 || transfer_memory_owner == nullptr) {
        seven_sixaxis_lifo.buffer_count = 0;
        seven_sixaxis_lifo.buffer_tail = 0;
        return;
    }

    const auto& last_entry = seven_sixaxis_lifo.ReadCurrentEntry().state;
    next_seven_sixaxis_state.sampling_number = last_entry.sampling_number + 1;

    const auto motion_status = console->GetMotion();
    last_global_timestamp = core_timing.GetGlobalTimeNs().count();

    // This value increments every time the switch goes to sleep
    next_seven_sixaxis_state.unknown = 1;
    next_seven_sixaxis_state.timestamp = last_global_timestamp - last_saved_timestamp;
    next_seven_sixaxis_state.accel = motion_status.accel;
    next_seven_sixaxis_state.gyro = motion_status.gyro;
    next_seven_sixaxis_state.quaternion = {
        motion_status.quaternion[1],
        motion_status.quaternion[0],
        -motion_status.quaternion[3],
        -motion_status.quaternion[2],
    };
    seven_sixaxis_lifo.WriteNextEntry(next_seven_sixaxis_state);
    transfer_memory_owner->GetMemory().WriteBlock(transfer_memory, &seven_sixaxis_lifo,
                                                  sizeof(seven_sixaxis_lifo));
}

void SevenSixAxis::SetTransferMemoryAddress(Common::ProcessAddress t_mem, Kernel::KProcess* owner) {
    transfer_memory = t_mem;
    transfer_memory_owner = owner;
}

void SevenSixAxis::ResetTimestamp() {
    last_saved_timestamp = last_global_timestamp;
}

namespace {
constexpr u64 ImuRingSize = 0x288;
constexpr u64 ImuRingWriteIndex = 0x280;
constexpr u64 ImuRingSamples = 16;
constexpr u64 ImuRightRings = 0x1950;
constexpr u64 ImuConsoleRing = 0x32a0;
constexpr u64 ImuSampleInterval = 5'000'000;
constexpr u64 ImuMergedInterval = 4'000'000;
constexpr s64 ImuMaxSampleAge = 100'000;
constexpr f32 MetersPerSecondSquaredPerG = 9.80665f;
constexpr f32 RadiansPerTurn = 6.2831855f;
} // namespace

NpadImu::NpadImu(Core::System& system_) : system{system_} {}

NpadImu::~NpadImu() = default;

void NpadImu::Enable(Common::ProcessAddress address, Kernel::KProcess* owner) {
    std::scoped_lock lock{mutex};
    left_rings = {};
    right_rings = {};
    console_ring = {};
    buffer = address;
    buffer_owner = owner;
    auto& hid_core = system.HIDCore();
    for (u64 index = 0; index < left_rings.size(); ++index) {
        if (auto* const controller =
                hid_core.GetEmulatedController(static_cast<Core::HID::NpadIdType>(index))) {
            controller->DiscardMotionSamples(Core::HID::MotionSampleReader::NpadImu);
        }
    }
    if (auto* const handheld = hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld)) {
        handheld->DiscardMotionSamples(Core::HID::MotionSampleReader::NpadImu);
    }
}

void NpadImu::Disable() {
    std::scoped_lock lock{mutex};
    buffer = {};
    buffer_owner = nullptr;
}

void NpadImu::WriteSample(Common::ProcessAddress ring_address, Ring& ring,
                          const Core::HID::ControllerMotion& motion, u64 delta_time) {
    const Sample sample{
        .sampling_number = ++ring.sampling_number,
        .delta_time = delta_time,
        .accel = motion.accel * MetersPerSecondSquaredPerG,
        .gyro = motion.gyro * RadiansPerTurn,
    };
    auto& memory = buffer_owner->GetMemory();
    memory.WriteBlock(ring_address + ring.write_index * sizeof(Sample), &sample, sizeof(sample));
    ring.write_index = (ring.write_index + 1) % ImuRingSamples;
    memory.Write64(ring_address + ImuRingWriteIndex, ring.write_index);
}

void NpadImu::WriteSamples(Common::ProcessAddress ring_address, Ring& ring,
                           Core::HID::EmulatedController& controller, std::size_t motion_index) {
    if (!controller.HasMotionSampleStream(motion_index)) {
        WriteSample(ring_address, ring, controller.GetMotions()[motion_index],
                    ImuSampleInterval);
        return;
    }
    std::array<Core::HID::ControllerMotionSample, ImuRingSamples / 2> samples{};
    const std::size_t count = controller.PopMotionSamples(
        motion_index, samples, Core::HID::MotionSampleReader::NpadImu, ImuMaxSampleAge);
    for (std::size_t i = 0; i < count; ++i) {
        const u64 delta_us = samples[i].delta_timestamp;
        const u64 delta_time =
            delta_us == 0 || delta_us > 100'000 ? ImuSampleInterval : delta_us * 1000;
        ring.pending_time += delta_time;
        ring.pending_gyro += samples[i].motion.gyro * static_cast<f32>(delta_time);
        ring.pending_accel = samples[i].motion.accel;
        if (ring.pending_time >= ImuMergedInterval) {
            Core::HID::ControllerMotion merged{};
            merged.accel = ring.pending_accel;
            merged.gyro = ring.pending_gyro / static_cast<f32>(ring.pending_time);
            WriteSample(ring_address, ring, merged, ring.pending_time);
            ring.pending_time = 0;
            ring.pending_gyro = {};
        }
    }
}

void NpadImu::OnUpdate() {
    std::scoped_lock lock{mutex};
    if (buffer_owner == nullptr || !Settings::values.motion_enabled.GetValue()) {
        return;
    }
    auto& hid_core = system.HIDCore();
    for (u64 index = 0; index < left_rings.size(); ++index) {
        auto* const controller =
            hid_core.GetEmulatedController(static_cast<Core::HID::NpadIdType>(index));
        if (controller == nullptr || !controller->IsConnected()) {
            continue;
        }
        const auto left = buffer + index * ImuRingSize;
        const auto right = buffer + ImuRightRings + index * ImuRingSize;
        switch (controller->GetNpadStyleIndex()) {
        case Core::HID::NpadStyleIndex::Fullkey:
        case Core::HID::NpadStyleIndex::JoyconLeft:
            WriteSamples(left, left_rings[index], *controller, 0);
            break;
        case Core::HID::NpadStyleIndex::JoyconRight:
            WriteSamples(right, right_rings[index], *controller, 1);
            break;
        case Core::HID::NpadStyleIndex::JoyconDual:
            WriteSamples(left, left_rings[index], *controller, 0);
            WriteSamples(right, right_rings[index], *controller, 1);
            break;
        default:
            break;
        }
    }
    auto* const handheld = hid_core.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    if (handheld != nullptr && handheld->IsConnected()) {
        WriteSamples(buffer + ImuConsoleRing, console_ring, *handheld, 0);
    }
}

} // namespace Service::HID
