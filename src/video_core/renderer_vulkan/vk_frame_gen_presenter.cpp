// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <exception>
#include <thread>

#include "video_core/frame_generation.h"
#include "video_core/renderer_vulkan/vk_frame_gen_presenter.h"
#include "video_core/renderer_vulkan/vk_frame_generator.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

[[nodiscard]] VkImageSubresourceLayers MakeSubresourceLayers() {
    return VkImageSubresourceLayers{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] VkImageBlit MakeBlit(s32 source_width, s32 source_height, s32 target_width,
                                   s32 target_height) {
    return VkImageBlit{
        .srcSubresource = MakeSubresourceLayers(),
        .srcOffsets = {{.x = 0, .y = 0, .z = 0}, {.x = source_width, .y = source_height, .z = 1}},
        .dstSubresource = MakeSubresourceLayers(),
        .dstOffsets = {{.x = 0, .y = 0, .z = 0}, {.x = target_width, .y = target_height, .z = 1}},
    };
}

[[nodiscard]] VkImageCopy MakeCopy(u32 source_width, u32 source_height, u32 target_width,
                                   u32 target_height) {
    return VkImageCopy{
        .srcSubresource = MakeSubresourceLayers(),
        .srcOffset = {.x = 0, .y = 0, .z = 0},
        .dstSubresource = MakeSubresourceLayers(),
        .dstOffset = {.x = 0, .y = 0, .z = 0},
        .extent =
            {
                .width = (std::min)(source_width, target_width),
                .height = (std::min)(source_height, target_height),
                .depth = 1,
            },
    };
}

[[nodiscard]] VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags src_access,
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
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
}

} // Anonymous namespace

FrameGenPresenter::FrameGenPresenter(PresentManager& manager_) : manager{manager_} {
    const auto& dld = manager.device.GetLogical();
    generator = std::make_unique<FrameGenerator>(manager.instance, manager.device, manager.scheduler);
    command_pool = dld.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = manager.device.GetGraphicsFamily(),
    });
    command_buffers = command_pool.Allocate(GENERATED_SLOTS);
    for (auto& fence : fences) {
        fence = dld.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
    }
}

FrameGenPresenter::~FrameGenPresenter() {
    for (const auto& fence : fences) {
        void(fence.Wait());
    }
}

bool FrameGenPresenter::UsePresentThread() {
    if (!VideoCore::FrameGeneration::Enabled()) {
        return false;
    }
    if (!manager.present_thread.joinable()) {
        PresentManager* const owner = &manager;
        manager.present_thread =
            std::jthread([owner](std::stop_token token) { owner->PresentThread(token); });
    }
    return true;
}

bool FrameGenPresenter::OnPresentThread() const {
    return std::this_thread::get_id() == manager.present_thread.get_id();
}

bool FrameGenPresenter::Present(Frame* frame) {
    const u32 generated =
        manager.transfer_mode == PresentManager::TransferMode::Blit ? generator->Prepare(frame->width, frame->height) : 0;
    if (generated > 0) {
        PresentWithFrameGeneration(frame, generated);
        return true;
    }

    last_frame_time = {};
    if (manager.use_present_thread || !OnPresentThread()) {
        return false;
    }
    PresentImage(frame, Source{
                            .image = *frame->image,
                            .width = frame->width,
                            .height = frame->height,
                            .is_frame = true,
                            .wait_binary = *frame->render_ready,
                            .cmdbuf = *frame->cmdbuf,
                            .fence = *frame->present_done,
                            .frame_pacing = true,
                        });
    return true;
}

void FrameGenPresenter::PresentWithFrameGeneration(Frame* frame, u32 generated) {
    namespace FG = VideoCore::FrameGeneration;
    using Clock = std::chrono::steady_clock;

    bool copied = false;
    bool usable = false;
    bool failed = false;
    try {
        usable = generator->Submit(*frame, copied);
    } catch (const std::exception& e) {
        generator->Fail(e.what());
        failed = true;
    }

    const auto now = Clock::now();
    if (last_frame_time != Clock::time_point{}) {
        const auto delta =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_frame_time);
        if (delta > std::chrono::milliseconds{2} && delta < std::chrono::milliseconds{250}) {
            frame_interval = (frame_interval * 7 + delta) / 8;
        }
    }
    last_frame_time = now;
    const auto step = frame_interval / (generated + 1);
    auto present_time = now;

    u32 shown = 0;
    if (usable && !failed) {
        for (u32 i = 0; i < generated; ++i) {
            if (i > 0) {
                WaitForPresentTime(present_time);
            }
            if (NewerFrameWaiting()) {
                break;
            }
            const std::size_t index = slot;
            slot = (slot + 1) % GENERATED_SLOTS;
            const vk::Fence& fence = fences[index];
            void(fence.Wait());
            fence.Reset();

            PresentImage(frame, Source{
                                    .image = generator->GeneratedImage(i),
                                    .width = frame->width,
                                    .height = frame->height,
                                    .is_frame = false,
                                    .wait_timeline = generator->SyncSemaphore(),
                                    .wait_value = generator->GeneratedReady(i),
                                    .cmdbuf = command_buffers[index],
                                    .fence = *fence,
                                    .frame_pacing = false,
                                });
            present_time += step;
            ++shown;
        }
        WaitForPresentTime(present_time);
    }

    Source real{
        .image = *frame->image,
        .width = frame->width,
        .height = frame->height,
        .is_frame = true,
        .cmdbuf = *frame->cmdbuf,
        .fence = *frame->present_done,
        .frame_pacing = true,
    };
    if (!copied) {
        real.wait_binary = *frame->render_ready;
    } else if (!failed) {
        real.wait_timeline = generator->SyncSemaphore();
        real.wait_value = generator->SourceReady();
    }
    PresentImage(frame, real);
    FG::CountPresented(1, shown);
}

bool FrameGenPresenter::NewerFrameWaiting() {
    std::scoped_lock lock{manager.queue_mutex};
    return !manager.present_queue.empty();
}

void FrameGenPresenter::WaitForPresentTime(std::chrono::steady_clock::time_point target) {
    using namespace std::chrono_literals;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= target) {
            return;
        }
        if (NewerFrameWaiting()) {
            return;
        }
        const auto remaining = target - now;
        if (remaining > 2ms) {
            std::this_thread::sleep_for((std::min)(
                std::chrono::duration_cast<std::chrono::nanoseconds>(remaining - 1500us),
                std::chrono::nanoseconds{4ms}));
        } else {
            std::this_thread::yield();
        }
    }
}

void FrameGenPresenter::PresentImage(Frame* frame, const Source& source) {
    Swapchain& swapchain = manager.swapchain;
    const Device& device = manager.device;
    while (swapchain.AcquireNextImage(!OnPresentThread(), source.frame_pacing)) {
        manager.RecreateSwapchain(frame);
    }

    const vk::CommandBuffer cmdbuf{source.cmdbuf, device.GetDispatchLoader()};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });

    const VkImage image{swapchain.CurrentImage()};
    const VkExtent2D extent = swapchain.GetExtent();
    const VkImageLayout source_layout =
        source.is_frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    const std::array pre_barriers{
        MakeBarrier(image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        MakeBarrier(source.image,
                    source.is_frame ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                    : VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, source_layout),
    };
    const std::array post_barriers{
        MakeBarrier(image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
        MakeBarrier(source.image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                    source_layout, VK_IMAGE_LAYOUT_GENERAL),
    };

    cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, {}, {}, {}, pre_barriers);

    if (manager.transfer_mode == PresentManager::TransferMode::Blit) {
        cmdbuf.BlitImage(source.image, source_layout, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         MakeBlit(static_cast<s32>(source.width), static_cast<s32>(source.height),
                                  static_cast<s32>(extent.width), static_cast<s32>(extent.height)),
                         VK_FILTER_LINEAR);
    } else {
        cmdbuf.CopyImage(source.image, source_layout, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         MakeCopy(source.width, source.height, extent.width, extent.height));
    }

    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, {},
                           {}, {}, post_barriers);

    cmdbuf.End();

    const VkSemaphore present_semaphore = swapchain.CurrentPresentSemaphore();
    const VkSemaphore render_semaphore = swapchain.CurrentRenderSemaphore();

    std::array<VkSemaphore, 3> wait_semaphores{present_semaphore};
    std::array<u64, 3> wait_values{0};
    u32 wait_count = 1;
    if (source.wait_binary) {
        wait_semaphores[wait_count] = source.wait_binary;
        wait_values[wait_count] = 0;
        ++wait_count;
    }
    if (source.wait_timeline) {
        wait_semaphores[wait_count] = source.wait_timeline;
        wait_values[wait_count] = source.wait_value;
        ++wait_count;
    }
    static constexpr std::array<VkPipelineStageFlags, 3> wait_stage_masks{
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
    };

    const VkTimelineSemaphoreSubmitInfo timeline_info{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = wait_count,
        .pWaitSemaphoreValues = wait_values.data(),
        .signalSemaphoreValueCount = 0,
        .pSignalSemaphoreValues = nullptr,
    };
    const VkCommandBuffer raw_cmdbuf = *cmdbuf;
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = source.wait_timeline ? &timeline_info : nullptr,
        .waitSemaphoreCount = wait_count,
        .pWaitSemaphores = wait_semaphores.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &raw_cmdbuf,
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &render_semaphore,
    };

    {
        std::scoped_lock submit_lock{manager.scheduler.submit_mutex};
        switch (const VkResult result = device.GetGraphicsQueue().Submit(submit_info, source.fence)) {
        case VK_SUCCESS:
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
    }

    swapchain.Present(render_semaphore);
}

} // namespace Vulkan
