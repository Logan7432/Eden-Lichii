/* SPDX-License-Identifier: GPL-3.0-or-later */

// Eden: trimmed to wrap an existing Vulkan device (see vulkan.hpp).

#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace vk;

namespace {
    template<typename T>
    T ipa(PFN_vkGetInstanceProcAddr mpa, VkInstance instance, const char* name) {
        T func = reinterpret_cast<T>(mpa(instance, name));
        if (!func)
            throw ls::vulkan_error("failed to get instance proc addr for " + std::string(name));
        return func;
    }

    template<typename T>
    T dpa(const VulkanInstanceFuncs& funcs, VkDevice device, const char* name,
            const char* fallback = nullptr) {
        T func = reinterpret_cast<T>(funcs.GetDeviceProcAddr(device, name));
        if (!func && fallback)
            func = reinterpret_cast<T>(funcs.GetDeviceProcAddr(device, fallback));
        if (!func)
            throw ls::vulkan_error("failed to get device proc addr for " + std::string(name));
        return func;
    }

    /// create a command pool
    ls::owned_ptr<VkCommandPool> createCommandPool(const VulkanDeviceFuncs& fd,
            VkDevice device, uint32_t cfi) {
        VkCommandPool handle{};

        const VkCommandPoolCreateInfo cmdpoolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = cfi
        };
        auto res = fd.CreateCommandPool(device, &cmdpoolInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateCommandPool() failed");

        return ls::owned_ptr<VkCommandPool>(
            new VkCommandPool(handle),
            [dev = device, defunc = fd.DestroyCommandPool](VkCommandPool& pool) {
                defunc(dev, pool, VK_NULL_HANDLE);
            }
        );
    }

    /// try to read the pipeline cache from file
    void readCacheFile(const std::filesystem::path& cachefile, std::vector<uint8_t>& data) {
        std::ifstream file(cachefile, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return;

        const std::streamsize size = static_cast<std::streamsize>(file.tellg());
        data = std::vector<uint8_t>(static_cast<size_t>(size));

        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(data.data()), size))
            data.clear();
    }

    /// create a pipeline cache
    ls::owned_ptr<VkPipelineCache> createPipelineCache(
            const VulkanDeviceFuncs& fd, VkDevice device,
            const std::optional<std::filesystem::path>& cachefile) {
        VkPipelineCache handle{};

        std::vector<uint8_t> cache{};
        std::error_code ec;
        if (cachefile && std::filesystem::exists(*cachefile, ec))
            readCacheFile(*cachefile, cache);

        VkPipelineCacheCreateInfo pipelineCacheInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .initialDataSize = cache.size(),
            .pInitialData = cache.data()
        };
        auto res = fd.CreatePipelineCache(device, &pipelineCacheInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS && !cache.empty()) {
            // stale or foreign cache data, start from scratch
            pipelineCacheInfo.initialDataSize = 0;
            pipelineCacheInfo.pInitialData = nullptr;
            res = fd.CreatePipelineCache(device, &pipelineCacheInfo, VK_NULL_HANDLE, &handle);
        }
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreatePipelineCache() failed");

        return ls::owned_ptr<VkPipelineCache>(
            new VkPipelineCache(handle),
            [dev = device, defunc = fd.DestroyPipelineCache](VkPipelineCache& cache) {
                defunc(dev, cache, VK_NULL_HANDLE);
            }
        );
    }
}

/// initialize vulkan instance function pointers
VulkanInstanceFuncs vk::initVulkanInstanceFuncs(VkInstance i, PFN_vkGetInstanceProcAddr mpa) {
    return {
        .GetPhysicalDeviceProperties2 = ipa<PFN_vkGetPhysicalDeviceProperties2>(mpa, i,
            "vkGetPhysicalDeviceProperties2"),
        .GetPhysicalDeviceMemoryProperties = ipa<PFN_vkGetPhysicalDeviceMemoryProperties>(mpa, i,
            "vkGetPhysicalDeviceMemoryProperties"),
        .GetDeviceProcAddr = ipa<PFN_vkGetDeviceProcAddr>(mpa, i, "vkGetDeviceProcAddr"),
    };
}

/// initialize vulkan device function pointers
VulkanDeviceFuncs vk::initVulkanDeviceFuncs(const VulkanInstanceFuncs& f, VkDevice d) {
    return {
        .DeviceWaitIdle = dpa<PFN_vkDeviceWaitIdle>(f, d, "vkDeviceWaitIdle"),
        .CreateCommandPool = dpa<PFN_vkCreateCommandPool>(f, d, "vkCreateCommandPool"),
        .DestroyCommandPool = dpa<PFN_vkDestroyCommandPool>(f, d, "vkDestroyCommandPool"),
        .CreateDescriptorPool = dpa<PFN_vkCreateDescriptorPool>(f, d, "vkCreateDescriptorPool"),
        .DestroyDescriptorPool = dpa<PFN_vkDestroyDescriptorPool>(f, d, "vkDestroyDescriptorPool"),
        .CreateBuffer = dpa<PFN_vkCreateBuffer>(f, d, "vkCreateBuffer"),
        .DestroyBuffer = dpa<PFN_vkDestroyBuffer>(f, d, "vkDestroyBuffer"),
        .GetBufferMemoryRequirements = dpa<PFN_vkGetBufferMemoryRequirements>(f, d,
            "vkGetBufferMemoryRequirements"),
        .AllocateMemory = dpa<PFN_vkAllocateMemory>(f, d, "vkAllocateMemory"),
        .FreeMemory = dpa<PFN_vkFreeMemory>(f, d, "vkFreeMemory"),
        .BindBufferMemory = dpa<PFN_vkBindBufferMemory>(f, d, "vkBindBufferMemory"),
        .MapMemory = dpa<PFN_vkMapMemory>(f, d, "vkMapMemory"),
        .UnmapMemory = dpa<PFN_vkUnmapMemory>(f, d, "vkUnmapMemory"),
        .AllocateCommandBuffers = dpa<PFN_vkAllocateCommandBuffers>(f, d,
            "vkAllocateCommandBuffers"),
        .FreeCommandBuffers = dpa<PFN_vkFreeCommandBuffers>(f, d, "vkFreeCommandBuffers"),
        .BeginCommandBuffer = dpa<PFN_vkBeginCommandBuffer>(f, d, "vkBeginCommandBuffer"),
        .EndCommandBuffer = dpa<PFN_vkEndCommandBuffer>(f, d, "vkEndCommandBuffer"),
        .CmdPipelineBarrier = dpa<PFN_vkCmdPipelineBarrier>(f, d, "vkCmdPipelineBarrier"),
        .CmdBlitImage = dpa<PFN_vkCmdBlitImage>(f, d, "vkCmdBlitImage"),
        .CmdClearColorImage = dpa<PFN_vkCmdClearColorImage>(f, d, "vkCmdClearColorImage"),
        .CmdBindPipeline = dpa<PFN_vkCmdBindPipeline>(f, d, "vkCmdBindPipeline"),
        .CmdBindDescriptorSets = dpa<PFN_vkCmdBindDescriptorSets>(f, d, "vkCmdBindDescriptorSets"),
        .CmdDispatch = dpa<PFN_vkCmdDispatch>(f, d, "vkCmdDispatch"),
        .CmdCopyBufferToImage = dpa<PFN_vkCmdCopyBufferToImage>(f, d, "vkCmdCopyBufferToImage"),
        .QueueSubmit = dpa<PFN_vkQueueSubmit>(f, d, "vkQueueSubmit"),
        .AllocateDescriptorSets = dpa<PFN_vkAllocateDescriptorSets>(f, d,
            "vkAllocateDescriptorSets"),
        .FreeDescriptorSets = dpa<PFN_vkFreeDescriptorSets>(f, d, "vkFreeDescriptorSets"),
        .UpdateDescriptorSets = dpa<PFN_vkUpdateDescriptorSets>(f, d, "vkUpdateDescriptorSets"),
        .CreateFence = dpa<PFN_vkCreateFence>(f, d, "vkCreateFence"),
        .DestroyFence = dpa<PFN_vkDestroyFence>(f, d, "vkDestroyFence"),
        .ResetFences = dpa<PFN_vkResetFences>(f, d, "vkResetFences"),
        .WaitForFences = dpa<PFN_vkWaitForFences>(f, d, "vkWaitForFences"),
        .CreateImage = dpa<PFN_vkCreateImage>(f, d, "vkCreateImage"),
        .DestroyImage = dpa<PFN_vkDestroyImage>(f, d, "vkDestroyImage"),
        .GetImageMemoryRequirements = dpa<PFN_vkGetImageMemoryRequirements>(f, d,
            "vkGetImageMemoryRequirements"),
        .BindImageMemory = dpa<PFN_vkBindImageMemory>(f, d, "vkBindImageMemory"),
        .CreateImageView = dpa<PFN_vkCreateImageView>(f, d, "vkCreateImageView"),
        .DestroyImageView = dpa<PFN_vkDestroyImageView>(f, d, "vkDestroyImageView"),
        .CreateSampler = dpa<PFN_vkCreateSampler>(f, d, "vkCreateSampler"),
        .DestroySampler = dpa<PFN_vkDestroySampler>(f, d, "vkDestroySampler"),
        .CreateSemaphore = dpa<PFN_vkCreateSemaphore>(f, d, "vkCreateSemaphore"),
        .DestroySemaphore = dpa<PFN_vkDestroySemaphore>(f, d, "vkDestroySemaphore"),
        .CreateShaderModule = dpa<PFN_vkCreateShaderModule>(f, d, "vkCreateShaderModule"),
        .DestroyShaderModule = dpa<PFN_vkDestroyShaderModule>(f, d, "vkDestroyShaderModule"),
        .CreateDescriptorSetLayout = dpa<PFN_vkCreateDescriptorSetLayout>(f, d,
            "vkCreateDescriptorSetLayout"),
        .DestroyDescriptorSetLayout = dpa<PFN_vkDestroyDescriptorSetLayout>(f, d,
            "vkDestroyDescriptorSetLayout"),
        .CreatePipelineLayout = dpa<PFN_vkCreatePipelineLayout>(f, d, "vkCreatePipelineLayout"),
        .DestroyPipelineLayout = dpa<PFN_vkDestroyPipelineLayout>(f, d, "vkDestroyPipelineLayout"),
        .CreatePipelineCache = dpa<PFN_vkCreatePipelineCache>(f, d, "vkCreatePipelineCache"),
        .DestroyPipelineCache = dpa<PFN_vkDestroyPipelineCache>(f, d, "vkDestroyPipelineCache"),
        .GetPipelineCacheData = dpa<PFN_vkGetPipelineCacheData>(f, d, "vkGetPipelineCacheData"),
        .CreateComputePipelines = dpa<PFN_vkCreateComputePipelines>(f, d, "vkCreateComputePipelines"),
        .DestroyPipeline = dpa<PFN_vkDestroyPipeline>(f, d, "vkDestroyPipeline"),

        .SignalSemaphoreKHR = dpa<PFN_vkSignalSemaphoreKHR>(f, d, "vkSignalSemaphoreKHR",
            "vkSignalSemaphore"),
        .WaitSemaphoresKHR = dpa<PFN_vkWaitSemaphoresKHR>(f, d, "vkWaitSemaphoresKHR",
            "vkWaitSemaphores"),

        .GetMemoryWin32HandleKHR = f.GetDeviceProcAddr(d, "vkGetMemoryWin32HandleKHR"),
    };
}

Vulkan::Vulkan(VkInstance instance, VkPhysicalDevice physdev, VkDevice device,
        PFN_vkGetInstanceProcAddr mpa, VkQueue queue, uint32_t queueFamily, bool fp16,
        QueueLock queueLock,
        const std::optional<std::filesystem::path>& cachefile) :
    instance(instance),
    instance_funcs(initVulkanInstanceFuncs(instance, mpa)),
    phys_dev(physdev),
    queueFamilyIdx(queueFamily),
    fp16(fp16),
    device(device),
    device_funcs(initVulkanDeviceFuncs(this->instance_funcs, device)),
    computeQueue(queue),
    queueLock(std::move(queueLock)),
    cmdPool(createCommandPool(this->device_funcs, device, queueFamily)),
    pipelineCache(createPipelineCache(this->device_funcs, device, cachefile)),
    cachefile(cachefile) {
}

std::optional<uint32_t> Vulkan::findMemoryTypeIndex(
        std::bitset<32> validTypes, bool hostVisibility) const {
    const VkMemoryPropertyFlags desiredProps = hostVisibility ?
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) :
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkPhysicalDeviceMemoryProperties props;
    this->instance_funcs.GetPhysicalDeviceMemoryProperties(this->phys_dev, &props);

    std::array<VkMemoryType, 32> memTypes = std::to_array(props.memoryTypes);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if (validTypes.test(i) && (memTypes.at(i).propertyFlags & desiredProps) == desiredProps)
            return i;

    return std::nullopt;
}

void Vulkan::submit(const VkSubmitInfo& info, VkFence fence) const {
    if (this->queueLock)
        this->queueLock(true);
    const auto res = this->device_funcs.QueueSubmit(this->computeQueue, 1, &info, fence);
    if (this->queueLock)
        this->queueLock(false);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkQueueSubmit() failed");
}

void Vulkan::waitIdle() const {
    if (this->queueLock)
        this->queueLock(true);
    this->device_funcs.DeviceWaitIdle(this->device);
    if (this->queueLock)
        this->queueLock(false);
}

void Vulkan::persistPipelineCache() const noexcept {
    if (!this->cachefile)
        return;

    size_t cacheSize{};
    auto res = this->device_funcs.GetPipelineCacheData(this->device,
        *this->pipelineCache,
        &cacheSize, nullptr);
    if (res != VK_SUCCESS)
        return;

    std::vector<uint8_t> cacheData(cacheSize);
    res = this->device_funcs.GetPipelineCacheData(this->device,
        *this->pipelineCache,
        &cacheSize, cacheData.data());
    if (res != VK_SUCCESS)
        return;

    std::error_code ec;
    std::filesystem::create_directories(this->cachefile->parent_path(), ec);
    std::ofstream file(*this->cachefile, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return;

    file.write(reinterpret_cast<const char*>(cacheData.data()),
        static_cast<std::streamsize>(cacheData.size()));
}
