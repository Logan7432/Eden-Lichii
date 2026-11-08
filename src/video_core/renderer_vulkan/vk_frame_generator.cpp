// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <exception>
#include <filesystem>
#include <optional>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/frame_generation.h"
#include "video_core/renderer_vulkan/vk_frame_generator.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

#ifdef EDEN_FRAME_GENERATION
#include "lsfg-vk-backend/lsfgvk.hpp"
#endif

namespace Vulkan {

namespace FG = VideoCore::FrameGeneration;

#ifdef EDEN_FRAME_GENERATION

namespace {

VkImageMemoryBarrier ImageBarrier(VkImage image, VkAccessFlags src_access,
                                  VkAccessFlags dst_access, VkImageLayout old_layout,
                                  VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

} // Anonymous namespace

FrameGenerator::FrameGenerator(const vk::Instance& instance_, const Device& device_,
                               Scheduler& scheduler_)
    : instance{instance_}, device{device_}, scheduler{scheduler_} {
    const auto& dld = device.GetLogical();
    command_pool = dld.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    command_buffers = command_pool.Allocate(COPY_SLOTS);
    for (auto& fence : fences) {
        fence = dld.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
    }
}

FrameGenerator::~FrameGenerator() {
    try {
        DestroyAll();
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Frame generation shutdown failed: {}", e.what());
    }
}

std::string FrameGenerator::SettingsKey() const {
    return Settings::values.lossless_dll_path.GetValue();
}

bool FrameGenerator::EnsureBackend() {
    const std::string dll_setting = Settings::values.lossless_dll_path.GetValue();
    if (backend && dll_setting == backend_dll_setting) {
        return true;
    }
    DestroyAll();

    const auto dll = FG::FindLosslessDll();
    if (!dll) {
        FG::SetState(FG::State::MissingDll,
                     "Lossless.dll was not found. Set its location in Settings > Graphics > "
                     "Frame Gen.");
        failed_key = SettingsKey();
        return false;
    }

    FG::SetState(FG::State::Starting, "Loading Lossless Scaling shaders");
    const auto& dld = device.GetDispatchLoader();
    VkQueue queue = VK_NULL_HANDLE;
    dld.vkGetDeviceQueue(*device.GetLogical(), device.GetGraphicsFamily(), 0, &queue);
    lsfgvk::backend::HostDevice host{
        .instance = *instance,
        .physicalDevice = static_cast<VkPhysicalDevice>(device.GetPhysical()),
        .device = *device.GetLogical(),
        .getInstanceProcAddr = dld.vkGetInstanceProcAddr,
        .queue = queue,
        .queueFamily = device.GetGraphicsFamily(),
        .float16 = device.IsFloat16Supported(),
        .queueLock =
            [this](bool lock) {
                if (lock) {
                    scheduler.submit_mutex.lock();
                } else {
                    scheduler.submit_mutex.unlock();
                }
            },
    };
    const auto cache = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) / "vulkan" /
                       "lsfg_pipeline_cache.bin";
    backend = std::make_unique<lsfgvk::backend::Instance>(host, *dll, true, cache);
    backend_dll_setting = dll_setting;
    LOG_INFO(Render_Vulkan, "Frame generation: loaded shaders from {}",
             Common::FS::PathToUTF8String(*dll));
    return true;
}

void FrameGenerator::DestroyContext() {
    if (context && backend) {
        backend->closeContext(*context);
    }
    context = nullptr;
    ticket.reset();
    width = height = generated = 0;
}

void FrameGenerator::DestroyAll() {
    DestroyContext();
    if (backend) {
        for (const auto& fence : fences) {
            void(fence.Wait());
        }
        backend.reset();
    }
    backend_dll_setting.clear();
}

void FrameGenerator::Fail(const std::string& message) {
    LOG_ERROR(Render_Vulkan, "Frame generation disabled: {}", message);
    try {
        DestroyAll();
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Frame generation cleanup failed: {}", e.what());
        context = nullptr;
        backend.reset();
    }
    FG::SetState(FG::State::Failed, message);
    failed_key = SettingsKey();
}

u32 FrameGenerator::Prepare(u32 frame_width, u32 frame_height) {
    if (!FG::Enabled()) {
        if (context || backend) {
            DestroyAll();
        }
        failed_key.clear();
        FG::SetState(FG::State::Off);
        return 0;
    }
    if (!device.HasTimelineSemaphore()) {
        FG::SetState(FG::State::Unsupported, "The GPU driver lacks timeline semaphores.");
        return 0;
    }
    if (!failed_key.empty()) {
        if (failed_key == SettingsKey()) {
            return 0;
        }
        failed_key.clear();
    }
    if (frame_width < 64 || frame_height < 64) {
        return 0;
    }

    try {
        if (!EnsureBackend()) {
            return 0;
        }
        const u32 wanted_generated = FG::Multiplier() - 1;
        const int wanted_flow =
            std::clamp(Settings::values.frame_generation_flow_scale.GetValue(), 25, 100);
        const bool wanted_performance = Settings::values.frame_generation_performance.GetValue();
        if (!context || width != frame_width || height != frame_height ||
            generated != wanted_generated || flow_scale != wanted_flow ||
            performance != wanted_performance) {
            DestroyContext();
            const float flow = 100.0f / static_cast<float>(wanted_flow);
            context = &backend->openContext(frame_width, frame_height, wanted_generated, false,
                                            flow, wanted_performance);
            width = frame_width;
            height = frame_height;
            generated = wanted_generated;
            flow_scale = wanted_flow;
            performance = wanted_performance;
            warmup = 1;
            LOG_INFO(Render_Vulkan, "Frame generation: {}x at {}x{}, flow scale {}%{}",
                     generated + 1, width, height, flow_scale,
                     performance ? ", performance mode" : "");
        }
    } catch (const std::exception& e) {
        Fail(e.what());
        return 0;
    }

    FG::SetState(FG::State::Active, fmt::format("{}x", generated + 1));
    return generated;
}

bool FrameGenerator::Submit(Frame& frame, bool& copied) {
    copied = false;
    ticket = std::make_unique<lsfgvk::backend::FrameTicket>(backend->nextFrame(*context));

    const std::size_t index = slot;
    slot = (slot + 1) % COPY_SLOTS;
    const vk::Fence& fence = fences[index];
    void(fence.Wait());
    fence.Reset();

    const vk::CommandBuffer cmdbuf{command_buffers[index], device.GetDispatchLoader()};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    const std::array pre{
        ImageBarrier(*frame.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        ImageBarrier(ticket->source, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           {}, {}, pre);
    const VkImageBlit region{
        .srcSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets{{0, 0, 0},
                    {static_cast<s32>(frame.width), static_cast<s32>(frame.height), 1}},
        .dstSubresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets{{0, 0, 0}, {static_cast<s32>(width), static_cast<s32>(height), 1}},
    };
    cmdbuf.BlitImage(*frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ticket->source,
                     VK_IMAGE_LAYOUT_GENERAL, region, VK_FILTER_NEAREST);
    const std::array post{
        ImageBarrier(*frame.image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        ImageBarrier(ticket->source, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, {}, {}, post);
    cmdbuf.End();

    const VkSemaphore wait_semaphore = *frame.render_ready;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSemaphore signal_semaphore = backend->syncSemaphore(*context);
    const u64 wait_value = 0;
    const u64 signal_value = ticket->sourceReady;
    const VkTimelineSemaphoreSubmitInfo timeline{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &wait_value,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &signal_value,
    };
    const VkCommandBuffer raw_cmdbuf = *cmdbuf;
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &wait_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &raw_cmdbuf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &signal_semaphore,
    };
    {
        std::scoped_lock lock{scheduler.submit_mutex};
        vk::Check(device.GetGraphicsQueue().Submit(submit, *fence));
    }
    copied = true;

    backend->scheduleFrames(*context);

    if (warmup > 0) {
        --warmup;
        return false;
    }
    return true;
}

VkImage FrameGenerator::GeneratedImage(u32 index) const {
    return backend->destinationImage(*context, index);
}

u64 FrameGenerator::GeneratedReady(u32 index) const {
    return ticket->generatedReady.at(index);
}

u64 FrameGenerator::SourceReady() const {
    return ticket->sourceReady;
}

VkSemaphore FrameGenerator::SyncSemaphore() const {
    return backend->syncSemaphore(*context);
}

#else

} // namespace Vulkan

namespace lsfgvk::backend {
class Instance {};
struct FrameTicket {};
} // namespace lsfgvk::backend

namespace Vulkan {

FrameGenerator::FrameGenerator(const vk::Instance& instance_, const Device& device_,
                               Scheduler& scheduler_)
    : instance{instance_}, device{device_}, scheduler{scheduler_} {}

FrameGenerator::~FrameGenerator() = default;

u32 FrameGenerator::Prepare(u32, u32) {
    FG::SetState(FG::Enabled() ? FG::State::Unsupported : FG::State::Off,
                 "Frame generation isn't available in this build.");
    return 0;
}

bool FrameGenerator::Submit(Frame&, bool& copied) {
    copied = false;
    return false;
}

VkImage FrameGenerator::GeneratedImage(u32) const {
    return VK_NULL_HANDLE;
}

u64 FrameGenerator::GeneratedReady(u32) const {
    return 0;
}

u64 FrameGenerator::SourceReady() const {
    return 0;
}

VkSemaphore FrameGenerator::SyncSemaphore() const {
    return VK_NULL_HANDLE;
}

void FrameGenerator::Fail(const std::string& message) {
    FG::SetState(FG::State::Failed, message);
}

#endif

} // namespace Vulkan
