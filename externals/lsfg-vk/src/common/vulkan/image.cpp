/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <bitset>
#include <optional>

#include <vulkan/vulkan_core.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

using namespace vk;

namespace {
    /// create a image
    ls::owned_ptr<VkImage> createImage(const vk::Vulkan& vk,
            VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, bool exportable) {
        VkImage handle{};

#ifdef _WIN32
        const VkExternalMemoryImageCreateInfo externalInfo{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
        };
        const void* next = exportable ? &externalInfo : nullptr;
#else
        const void* next = nullptr;
        (void)exportable;
#endif
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = next,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {
                .width = extent.width,
                .height = extent.height,
                .depth = 1
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        auto res = vk.df().CreateImage(vk.dev(), &imageInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateImage() failed");

        return ls::owned_ptr<VkImage>(
            new VkImage(handle),
            [dev = vk.dev(), defunc = vk.df().DestroyImage](VkImage& image) {
                defunc(dev, image, VK_NULL_HANDLE);
            }
        );
    }
    /// allocate memory for a image
    ls::owned_ptr<VkDeviceMemory> allocateMemory(const vk::Vulkan& vk, VkImage image,
            bool exportable, uint64_t& size) {
        VkDeviceMemory handle{};

        VkMemoryRequirements reqs{};
        vk.df().GetImageMemoryRequirements(vk.dev(), image, &reqs);

        auto mti = vk.findMemoryTypeIndex(
            reqs.memoryTypeBits,
            false
        );
        if (!mti.has_value())
            throw ls::vulkan_error("no suitable memory type found for image");

        // Exported memory is a dedicated allocation, as other APIs expect it
        const VkMemoryDedicatedAllocateInfo dedicatedInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = image,
        };
#ifdef _WIN32
        const VkExportMemoryAllocateInfo exportInfo{
            .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicatedInfo,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
        };
        const void* next = exportable ? &exportInfo : nullptr;
#else
        const void* next = nullptr;
#endif
        const VkMemoryAllocateInfo memoryInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = next,
            .allocationSize = reqs.size,
            .memoryTypeIndex = *mti
        };
        size = reqs.size;
        auto res = vk.df().AllocateMemory(vk.dev(), &memoryInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkAllocateMemory() failed");

        res = vk.df().BindImageMemory(vk.dev(), image, handle, 0);
        if (res != VK_SUCCESS) {
            vk.df().FreeMemory(vk.dev(), handle, VK_NULL_HANDLE);
            throw ls::vulkan_error(res, "vkBindImageMemory() failed");
        }

        return ls::owned_ptr<VkDeviceMemory>(
            new VkDeviceMemory(handle),
            [dev = vk.dev(), defunc = vk.df().FreeMemory](VkDeviceMemory& memory) {
                defunc(dev, memory, VK_NULL_HANDLE);
            }
        );
    }
    /// create an image view
    ls::owned_ptr<VkImageView> createImageView(const vk::Vulkan& vk,
            VkImage image, VkFormat format) {
        VkImageView handle{};

        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        auto res = vk.df().CreateImageView(vk.dev(), &viewInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateImageView() failed");

        return ls::owned_ptr<VkImageView>(
            new VkImageView(handle),
            [dev = vk.dev(), defunc = vk.df().DestroyImageView](VkImageView& view) {
                defunc(dev, view, VK_NULL_HANDLE);
            }
        );
    }
}

Image::Image(const vk::Vulkan& vk,
            VkExtent2D extent,
            VkFormat format,
            VkImageUsageFlags usage,
            bool exportable) :
        image(createImage(vk, extent, format, usage, exportable)),
        memory(allocateMemory(vk, *this->image, exportable, this->size)),
        view(createImageView(vk, *this->image, format)),
        extent(extent),
        exportable(exportable) {
}

void* Image::exportWin32Handle(const vk::Vulkan& vk) const {
#ifdef _WIN32
    if (!this->exportable || !vk.df().GetMemoryWin32HandleKHR)
        throw ls::vulkan_error("image memory is not exportable");
    const auto getHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        vk.df().GetMemoryWin32HandleKHR);
    const VkMemoryGetWin32HandleInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
        .memory = *this->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
    };
    HANDLE handle{};
    const auto res = getHandle(vk.dev(), &info, &handle);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkGetMemoryWin32HandleKHR() failed");
    return handle;
#else
    (void)vk;
    throw ls::vulkan_error("memory export is only implemented on Windows");
#endif
}
