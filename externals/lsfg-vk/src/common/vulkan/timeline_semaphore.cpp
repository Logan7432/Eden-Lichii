/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/timeline_semaphore.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <cstdint>
#include <optional>

#include <vulkan/vulkan_core.h>

using namespace vk;

namespace {
    /// create a timeline semaphore
    ls::owned_ptr<VkSemaphore> createTimelineSemaphore(const vk::Vulkan& vk, uint32_t initial) {
        VkSemaphore handle{};

        const VkSemaphoreTypeCreateInfo typeInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = initial
        };
        const VkSemaphoreCreateInfo semaphoreInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &typeInfo,
        };
        auto res = vk.df().CreateSemaphore(vk.dev(), &semaphoreInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateSemaphore() failed");

        return ls::owned_ptr<VkSemaphore>(
            new VkSemaphore(handle),
            [dev = vk.dev(), defunc = vk.df().DestroySemaphore](VkSemaphore& semaphore) {
                defunc(dev, semaphore, VK_NULL_HANDLE);
            }
        );
    }
}

TimelineSemaphore::TimelineSemaphore(const vk::Vulkan& vk, uint32_t initial)
    : semaphore(createTimelineSemaphore(vk, initial)) {}

void TimelineSemaphore::signal(const vk::Vulkan& vk, uint64_t value) const {
    const VkSemaphoreSignalInfo signalInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = *this->semaphore,
        .value = value
    };
    auto res = vk.df().SignalSemaphoreKHR(vk.dev(), &signalInfo);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkSignalSemaphore() failed");
}

bool TimelineSemaphore::wait(const vk::Vulkan& vk, uint64_t value, uint64_t timeout) const {
    const VkSemaphoreWaitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &*this->semaphore,
        .pValues = &value
    };
    auto res = vk.df().WaitSemaphoresKHR(vk.dev(), &waitInfo, timeout);
    if (res != VK_SUCCESS && res != VK_TIMEOUT)
        throw ls::vulkan_error(res, "vkWaitSemaphores() failed");

    return res == VK_SUCCESS;
}
