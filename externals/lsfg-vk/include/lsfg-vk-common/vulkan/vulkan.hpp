/* SPDX-License-Identifier: GPL-3.0-or-later */

// Eden: trimmed to run on an existing, externally managed Vulkan device (no own instance,
// no external memory). Queue access goes through a host provided lock.

#pragma once

#include "../helpers/pointers.hpp"

#include <bitset>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace vk {

    /// vulkan instance function pointers
    struct VulkanInstanceFuncs {
        PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
        PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
        PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    };

    /// initialize vulkan instance function pointers
    /// @param instance vulkan instance handle
    /// @param mpa function to get instance proc addresses
    /// @return initialized function pointers
    VulkanInstanceFuncs initVulkanInstanceFuncs(VkInstance instance, PFN_vkGetInstanceProcAddr mpa);

    /// vulkan device function pointers
    struct VulkanDeviceFuncs {
        PFN_vkDeviceWaitIdle DeviceWaitIdle;
        PFN_vkCreateCommandPool CreateCommandPool;
        PFN_vkDestroyCommandPool DestroyCommandPool;
        PFN_vkCreateDescriptorPool CreateDescriptorPool;
        PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
        PFN_vkCreateBuffer CreateBuffer;
        PFN_vkDestroyBuffer DestroyBuffer;
        PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
        PFN_vkAllocateMemory AllocateMemory;
        PFN_vkFreeMemory FreeMemory;
        PFN_vkBindBufferMemory BindBufferMemory;
        PFN_vkMapMemory MapMemory;
        PFN_vkUnmapMemory UnmapMemory;
        PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
        PFN_vkFreeCommandBuffers FreeCommandBuffers;
        PFN_vkBeginCommandBuffer BeginCommandBuffer;
        PFN_vkEndCommandBuffer EndCommandBuffer;
        PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
        PFN_vkCmdBlitImage CmdBlitImage;
        PFN_vkCmdClearColorImage CmdClearColorImage;
        PFN_vkCmdBindPipeline CmdBindPipeline;
        PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
        PFN_vkCmdDispatch CmdDispatch;
        PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
        PFN_vkQueueSubmit QueueSubmit;
        PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
        PFN_vkFreeDescriptorSets FreeDescriptorSets;
        PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
        PFN_vkCreateFence CreateFence;
        PFN_vkDestroyFence DestroyFence;
        PFN_vkResetFences ResetFences;
        PFN_vkWaitForFences WaitForFences;
        PFN_vkCreateImage CreateImage;
        PFN_vkDestroyImage DestroyImage;
        PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
        PFN_vkBindImageMemory BindImageMemory;
        PFN_vkCreateImageView CreateImageView;
        PFN_vkDestroyImageView DestroyImageView;
        PFN_vkCreateSampler CreateSampler;
        PFN_vkDestroySampler DestroySampler;
        PFN_vkCreateSemaphore CreateSemaphore;
        PFN_vkDestroySemaphore DestroySemaphore;
        PFN_vkCreateShaderModule CreateShaderModule;
        PFN_vkDestroyShaderModule DestroyShaderModule;
        PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
        PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
        PFN_vkCreatePipelineLayout CreatePipelineLayout;
        PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
        PFN_vkCreatePipelineCache CreatePipelineCache;
        PFN_vkDestroyPipelineCache DestroyPipelineCache;
        PFN_vkGetPipelineCacheData GetPipelineCacheData;
        PFN_vkCreateComputePipelines CreateComputePipelines;
        PFN_vkDestroyPipeline DestroyPipeline;

        // timeline semaphores (core in 1.2, KHR extension before)
        PFN_vkSignalSemaphoreKHR SignalSemaphoreKHR;
        PFN_vkWaitSemaphoresKHR WaitSemaphoresKHR;

        // optional: vkGetMemoryWin32HandleKHR (VK_KHR_external_memory_win32), null if missing
        PFN_vkVoidFunction GetMemoryWin32HandleKHR;
    };

    /// initialize vulkan device function pointers
    /// @param fi instance function pointers
    /// @param device logical device handle
    /// @return initialized function pointers
    VulkanDeviceFuncs initVulkanDeviceFuncs(const VulkanInstanceFuncs& fi, VkDevice device);

    /// Lock guarding the (shared) host queue. Called around every queue operation.
    using QueueLock = std::function<void(bool lock)>;

    /// vulkan device wrapper around an existing, externally managed device
    class Vulkan {
    public:
        /// wrap an existing vulkan device
        /// @param instance vulkan instance handle
        /// @param physdev physical device handle
        /// @param device logical device handle
        /// @param mpa vkGetInstanceProcAddr of the instance
        /// @param queue queue to submit to (shared with the host)
        /// @param queueFamily family of @p queue
        /// @param fp16 whether shaderFloat16 is enabled on the device
        /// @param queueLock lock serializing access to @p queue
        /// @param cachefile optional path to pipeline cache file
        /// @throws ls::vulkan_error on failure
        Vulkan(VkInstance instance, VkPhysicalDevice physdev, VkDevice device,
            PFN_vkGetInstanceProcAddr mpa, VkQueue queue, uint32_t queueFamily, bool fp16,
            QueueLock queueLock,
            const std::optional<std::filesystem::path>& cachefile = std::nullopt);

        /// find a memory type index
        /// @param validTypes bitset of valid memory types
        /// @param hostVisibility whether the memory should be host visible
        /// @return the memory type index
        [[nodiscard]] std::optional<uint32_t> findMemoryTypeIndex(
            std::bitset<32> validTypes, bool hostVisibility) const;

        /// persist the pipeline cache to file, silently failing on error
        void persistPipelineCache() const noexcept;

        /// submit work to the shared queue
        /// @throws ls::vulkan_error on failure
        void submit(const VkSubmitInfo& info, VkFence fence) const;

        /// wait until the device is idle (takes the queue lock)
        void waitIdle() const;

        /// get the vulkan instance
        /// @return the instance handle
        [[nodiscard]] VkInstance inst() const { return this->instance; }
        /// get the vulkan device
        /// @return the device handle
        [[nodiscard]] VkDevice dev() const { return this->device; }
        /// get the physical device
        /// @return the physical device handle
        [[nodiscard]] VkPhysicalDevice physdev() const { return this->phys_dev; }
        /// get the command pool
        /// @return the command pool handle
        [[nodiscard]] const auto& cmdpool() const { return this->cmdPool.get(); }
        /// get the pipeline cache
        /// @return the pipeline cache handle
        [[nodiscard]] const auto& cache() const { return this->pipelineCache.get(); }
        /// get the queue
        /// @return the queue handle
        [[nodiscard]] VkQueue queue() const { return this->computeQueue; }

        /// check if fp16 is supported
        /// @return true if fp16 is supported
        [[nodiscard]] bool supportsFP16() const { return this->fp16; }

        /// get instance-level function pointers
        /// @return the instance function pointers
        [[nodiscard]] const auto& fi() const { return this->instance_funcs; }
        /// get device-level function pointers
        /// @return the device function pointers
        [[nodiscard]] const auto& df() const { return this->device_funcs; }
    private:
        VkInstance instance;
        VulkanInstanceFuncs instance_funcs;

        VkPhysicalDevice phys_dev;
        uint32_t queueFamilyIdx;
        bool fp16;

        VkDevice device;
        VulkanDeviceFuncs device_funcs;

        VkQueue computeQueue;
        QueueLock queueLock;

        ls::owned_ptr<VkCommandPool> cmdPool;
        ls::owned_ptr<VkPipelineCache> pipelineCache;
        std::optional<std::filesystem::path> cachefile;
    };
}
