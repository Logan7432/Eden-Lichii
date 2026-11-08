// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class FrameGenerator;
class PresentManager;
struct Frame;

class FrameGenPresenter {
public:
    explicit FrameGenPresenter(PresentManager& manager);
    ~FrameGenPresenter();

    FrameGenPresenter(const FrameGenPresenter&) = delete;
    FrameGenPresenter& operator=(const FrameGenPresenter&) = delete;

    bool UsePresentThread();

    bool Present(Frame* frame);

private:
    struct Source {
        VkImage image{};
        u32 width{};
        u32 height{};
        bool is_frame{};
        VkSemaphore wait_binary{};
        VkSemaphore wait_timeline{};
        u64 wait_value{};
        VkCommandBuffer cmdbuf{};
        VkFence fence{};
        bool frame_pacing{true};
    };

    void PresentWithFrameGeneration(Frame* frame, u32 generated);
    void PresentImage(Frame* frame, const Source& source);
    bool OnPresentThread() const;
    bool NewerFrameWaiting();
    void WaitForPresentTime(std::chrono::steady_clock::time_point target);

    static constexpr std::size_t GENERATED_SLOTS = 8;

    PresentManager& manager;
    std::unique_ptr<FrameGenerator> generator;
    vk::CommandPool command_pool;
    vk::CommandBuffers command_buffers;
    std::array<vk::Fence, GENERATED_SLOTS> fences;
    std::size_t slot{};
    std::chrono::steady_clock::time_point last_frame_time{};
    std::chrono::nanoseconds frame_interval{16'666'667};
};

} // namespace Vulkan
