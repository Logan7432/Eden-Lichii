// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <string>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace lsfgvk::backend {
class Instance;
class ContextImpl;
struct FrameTicket;
} // namespace lsfgvk::backend

namespace Vulkan {

class Device;
class Scheduler;
struct Frame;

class FrameGenerator {
public:
    explicit FrameGenerator(const vk::Instance& instance, const Device& device,
                            Scheduler& scheduler);
    ~FrameGenerator();

    FrameGenerator(const FrameGenerator&) = delete;
    FrameGenerator& operator=(const FrameGenerator&) = delete;

    u32 Prepare(u32 width, u32 height);

    bool Submit(Frame& frame, bool& copied);

    [[nodiscard]] VkImage GeneratedImage(u32 index) const;
    [[nodiscard]] u64 GeneratedReady(u32 index) const;
    [[nodiscard]] u64 SourceReady() const;
    [[nodiscard]] VkSemaphore SyncSemaphore() const;

    void Fail(const std::string& message);

private:
    bool EnsureBackend();
    void DestroyContext();
    void DestroyAll();
    std::string SettingsKey() const;

    const vk::Instance& instance;
    const Device& device;
    Scheduler& scheduler;

    std::unique_ptr<lsfgvk::backend::Instance> backend;
    lsfgvk::backend::ContextImpl* context = nullptr;
    std::unique_ptr<lsfgvk::backend::FrameTicket> ticket;

    u32 width = 0;
    u32 height = 0;
    u32 generated = 0;
    int flow_scale = 0;
    bool performance = false;
    u32 warmup = 0;

    std::string backend_dll_setting;
    std::string failed_key;

    static constexpr std::size_t COPY_SLOTS = 4;
    vk::CommandPool command_pool;
    vk::CommandBuffers command_buffers;
    std::array<vk::Fence, COPY_SLOTS> fences;
    std::size_t slot = 0;
};

} // namespace Vulkan
