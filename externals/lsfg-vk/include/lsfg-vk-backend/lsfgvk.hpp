/* SPDX-License-Identifier: GPL-3.0-or-later */

// Eden: in-process variant of the lsfg-vk backend. Frame generation runs on the host's own
// Vulkan device and queue; source/destination images and the timeline semaphore are owned by the
// backend and handed to the host, so no memory has to be shared between devices.

#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace lsfgvk::backend {

    class ContextImpl;
    class InstanceImpl;

    using Context = ContextImpl;

    ///
    /// Primitive exception class that deliveres a detailed error message
    ///
    class error : public std::runtime_error {
    public:
        explicit error(const std::string &msg, const std::exception &inner);
        explicit error(const std::string &msg);

        error(const error &) = default;
        error &operator=(const error &) = default;
        error(error &&) = default;
        error &operator=(error &&) = default;
        ~error() override;
    };

    ///
    /// Vulkan objects of the host that frame generation runs on.
    ///
    struct HostDevice {
        VkInstance instance{};
        VkPhysicalDevice physicalDevice{};
        VkDevice device{};
        PFN_vkGetInstanceProcAddr getInstanceProcAddr{};
        VkQueue queue{};
        uint32_t queueFamily{};
        /// Whether shaderFloat16 is enabled on the device
        bool float16{};
        /// Called with true before and false after every use of the queue
        std::function<void(bool lock)> queueLock;
    };

    ///
    /// What the host has to do for one real frame (see Instance::scheduleFrames).
    ///
    struct FrameTicket {
        /// Image the new real frame has to be copied into (VK_IMAGE_LAYOUT_GENERAL).
        VkImage source{};
        /// Index (0 or 1) of that source image.
        uint32_t sourceIndex{};
        /// Value to signal on the sync semaphore once the copy is done.
        uint64_t sourceReady{};
        /// Values that the sync semaphore reaches when the generated frames are ready.
        std::vector<uint64_t> generatedReady;
    };

    ///
    /// Memory of a context image exported for another API (exportable contexts only).
    ///
    struct ExportedImage {
        void* handle{};       ///< Opaque Win32 handle, owned (and to be closed) by the caller
        uint64_t size{};      ///< Allocation size (dedicated allocation)
        uint32_t width{};
        uint32_t height{};
    };

    ///
    /// Main entry point of the library
    ///
    class Instance {
    public:
        ///
        /// Create a lsfg-vk instance on the host's device
        ///
        /// @param host Host device and queue.
        /// @param shaderDllPath Path to the Lossless.dll file to load shaders from.
        /// @param allowLowPrecision Whether to load low-precision (FP16) shaders if supported.
        /// @param pipelineCache Optional file to persist the compute pipeline cache in.
        ///
        /// @throws backend::error on failure
        ///
        Instance(
            const HostDevice& host,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision,
            const std::optional<std::filesystem::path>& pipelineCache = std::nullopt
        );

        ///
        /// Open a frame generation context.
        ///
        /// Images are VK_FORMAT_R8G8B8A8_UNORM (or VK_FORMAT_R16G16B16A16_SFLOAT with hdr) and
        /// stay in VK_IMAGE_LAYOUT_GENERAL.
        ///
        /// @param width Width of the images.
        /// @param height Height of the images.
        /// @param generated Number of frames generated between two real frames.
        /// @param hdr Whether the images are HDR.
        /// @param flow Motion flow factor (1 / flow scale).
        /// @param perf Whether to enable performance mode.
        ///
        /// @throws backend::error on failure
        ///
        /// @param exportable Create the source and destination images exportable (Win32), so
        ///                   another API (OpenGL) can share them.
        Context& openContext(uint32_t width, uint32_t height, uint32_t generated,
            bool hdr, float flow, bool perf, bool exportable = false);

        /// Source image (0 or 1) and destination images of a context.
        [[nodiscard]] VkImage sourceImage(const Context& context, size_t i) const;

        /// Export a source (0, 1) or destination image of an exportable context.
        [[nodiscard]] ExportedImage exportSourceImage(const Context& context, size_t i) const;
        [[nodiscard]] ExportedImage exportDestinationImage(const Context& context, size_t i) const;

        ///
        /// What to do for the next real frame. Call before scheduleFrames().
        ///
        [[nodiscard]] FrameTicket nextFrame(const Context& context) const;

        ///
        /// Schedule a new set of generated frames. The host must have submitted (or submit later
        /// on the same queue... prefer before) the copy of the new frame into ticket.source,
        /// signaling ticket.sourceReady on the sync semaphore.
        ///
        /// @param context Context to use.
        /// @throws backend::error on failure
        ///
        void scheduleFrames(Context& context);

        /// Destination image of the i-th generated frame.
        [[nodiscard]] VkImage destinationImage(const Context& context, size_t i) const;

        /// Timeline semaphore used for synchronization with the host.
        [[nodiscard]] VkSemaphore syncSemaphore(const Context& context) const;

        ///
        /// Close a frame generation context (waits for the device to be idle)
        ///
        void closeContext(const Context& context);

        // Non-copyable and non-movable
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;
        Instance(Instance&&) = delete;
        Instance& operator=(Instance&&) = delete;
        virtual ~Instance();
    private:
        std::unique_ptr<InstanceImpl> m_impl;

        std::vector<std::unique_ptr<Context>> m_contexts;
    };

}
