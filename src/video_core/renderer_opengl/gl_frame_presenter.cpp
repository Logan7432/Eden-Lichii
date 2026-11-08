// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/vulkan_common/vulkan.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/frame_generation.h"
#include "video_core/renderer_opengl/gl_frame_presenter.h"
#include "video_core/vulkan_common/vulkan_library.h"

#ifdef EDEN_FRAME_GENERATION
#include "lsfg-vk-backend/lsfgvk.hpp"
#endif

namespace OpenGL {

namespace FG = VideoCore::FrameGeneration;

#if defined(EDEN_FRAME_GENERATION) && defined(_WIN32)

namespace {

[[noreturn]] void Throw(const std::string& message) {
    throw std::runtime_error(message);
}

void Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        Throw(fmt::format("{} failed ({})", what, static_cast<int>(result)));
    }
}

} // Anonymous namespace

class FrameGeneratorGL {
public:
    FrameGeneratorGL() = default;
    ~FrameGeneratorGL() {
        try {
            DestroyAll();
        } catch (const std::exception& e) {
            LOG_ERROR(Render_OpenGL, "Frame generation shutdown failed: {}", e.what());
        }
    }

    FrameGeneratorGL(const FrameGeneratorGL&) = delete;
    FrameGeneratorGL& operator=(const FrameGeneratorGL&) = delete;

    u32 Prepare(u32 frame_width, u32 frame_height) {
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
            const bool wanted_performance =
                Settings::values.frame_generation_performance.GetValue();
            if (!context || width != frame_width || height != frame_height ||
                generated != wanted_generated || flow_scale != wanted_flow ||
                performance != wanted_performance) {
                CreateContext(frame_width, frame_height, wanted_generated, wanted_flow,
                              wanted_performance);
            }
        } catch (const std::exception& e) {
            Fail(e.what());
            return 0;
        }
        FG::SetState(FG::State::Active, fmt::format("{}x", generated + 1));
        return generated;
    }

    bool Submit(GLuint read_framebuffer) {
        const auto ticket = backend->nextFrame(*context);

        WaitForVulkan();
        glBlitNamedFramebuffer(read_framebuffer, sources[ticket.sourceIndex].framebuffer, 0, 0,
                               static_cast<GLint>(width), static_cast<GLint>(height), 0, 0,
                               static_cast<GLint>(width), static_cast<GLint>(height),
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glSignalSemaphoreEXT(gl_done.gl, 0, nullptr, static_cast<GLuint>(textures.size()),
                             textures.data(), layouts.data());
        glFlush();

        const VkSemaphore sync = backend->syncSemaphore(*context);
        Submit(acquire_cmdbuf, gl_done.vk, VK_NULL_HANDLE, 0, sync, ticket.sourceReady);
        backend->scheduleFrames(*context);
        Submit(release_cmdbuf, VK_NULL_HANDLE, sync, ticket.generatedReady.back(), vk_done.vk, 0);
        vk_done_pending = true;

        WaitForVulkan();

        if (warmup > 0) {
            --warmup;
            return false;
        }
        return true;
    }

    [[nodiscard]] GLuint GeneratedFramebuffer(u32 index) const {
        return destinations.at(index).framebuffer;
    }

    void Fail(const std::string& message) {
        LOG_ERROR(Render_OpenGL, "Frame generation disabled: {}", message);
        try {
            DestroyAll();
        } catch (const std::exception& e) {
            LOG_ERROR(Render_OpenGL, "Frame generation cleanup failed: {}", e.what());
        }
        FG::SetState(FG::State::Failed, message);
        failed_key = SettingsKey();
    }

private:
    struct SharedImage {
        GLuint memory = 0;
        GLuint texture = 0;
        GLuint framebuffer = 0;
        void* handle = nullptr;
    };

    struct SharedSemaphore {
        VkSemaphore vk = VK_NULL_HANDLE;
        GLuint gl = 0;
        void* handle = nullptr;
    };

    static std::string SettingsKey() {
        return Settings::values.lossless_dll_path.GetValue();
    }

    void WaitForVulkan() {
        if (!vk_done_pending) {
            return;
        }
        glWaitSemaphoreEXT(vk_done.gl, 0, nullptr, static_cast<GLuint>(textures.size()),
                           textures.data(), layouts.data());
        vk_done_pending = false;
    }

    template <typename T>
    T InstanceProc(const char* name) const {
        auto* proc = reinterpret_cast<T>(get_instance_proc(instance, name));
        if (!proc) {
            Throw(fmt::format("Vulkan function {} is missing", name));
        }
        return proc;
    }

    template <typename T>
    T DeviceProc(const char* name) const {
        auto* proc = reinterpret_cast<T>(get_device_proc(device, name));
        if (!proc) {
            Throw(fmt::format("Vulkan function {} is missing", name));
        }
        return proc;
    }

    bool EnsureBackend() {
        const std::string dll_setting = SettingsKey();
        if (backend && dll_setting == backend_dll_setting) {
            return true;
        }
        DestroyAll();

        if (!GLAD_GL_EXT_memory_object || !GLAD_GL_EXT_memory_object_win32 ||
            !GLAD_GL_EXT_semaphore || !GLAD_GL_EXT_semaphore_win32 ||
            !GLAD_GL_ARB_direct_state_access) {
            FG::SetState(FG::State::Unsupported,
                         "The OpenGL driver can't share images with Vulkan "
                         "(GL_EXT_memory_object_win32 / GL_EXT_semaphore_win32).");
            failed_key = dll_setting;
            return false;
        }
        const auto dll = FG::FindLosslessDll();
        if (!dll) {
            FG::SetState(FG::State::MissingDll,
                         "Lossless.dll was not found. Set its location in Settings > Graphics > "
                         "Frame Gen.");
            failed_key = dll_setting;
            return false;
        }

        FG::SetState(FG::State::Starting, "Loading Lossless Scaling shaders");
        CreateDevice();
        const lsfgvk::backend::HostDevice host{
            .instance = instance,
            .physicalDevice = physical_device,
            .device = device,
            .getInstanceProcAddr = get_instance_proc,
            .queue = queue,
            .queueFamily = queue_family,
            .float16 = float16,
            .queueLock = {},
        };
        const auto cache = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) / "opengl" /
                           "lsfg_pipeline_cache.bin";
        backend = std::make_unique<lsfgvk::backend::Instance>(host, *dll, true, cache);
        backend_dll_setting = dll_setting;
        LOG_INFO(Render_OpenGL, "Frame generation: loaded shaders from {}",
                 Common::FS::PathToUTF8String(*dll));
        return true;
    }

    void CreateDevice() {
        library = Vulkan::OpenLibrary();
        if (!library || !library->GetSymbol("vkGetInstanceProcAddr", &get_instance_proc)) {
            Throw("The Vulkan runtime could not be loaded");
        }

        const auto create_instance =
            reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"));
        if (!create_instance) {
            Throw("vkCreateInstance is missing");
        }
        const VkApplicationInfo app_info{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext = nullptr,
            .pApplicationName = "Eden frame generation",
            .applicationVersion = 1,
            .pEngineName = "Eden",
            .engineVersion = 1,
            .apiVersion = VK_API_VERSION_1_2,
        };
        const VkInstanceCreateInfo instance_info{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .pApplicationInfo = &app_info,
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = 0,
            .ppEnabledExtensionNames = nullptr,
        };
        Check(create_instance(&instance_info, nullptr, &instance), "vkCreateInstance");

        get_device_proc = InstanceProc<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        const auto enumerate_devices =
            InstanceProc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        const auto get_properties2 =
            InstanceProc<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
        const auto get_features2 =
            InstanceProc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
        const auto get_queue_families =
            InstanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                "vkGetPhysicalDeviceQueueFamilyProperties");
        const auto enumerate_extensions = InstanceProc<PFN_vkEnumerateDeviceExtensionProperties>(
            "vkEnumerateDeviceExtensionProperties");
        const auto create_device = InstanceProc<PFN_vkCreateDevice>("vkCreateDevice");

        std::array<GLubyte, GL_UUID_SIZE_EXT> gl_device_uuid{};
        glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, 0, gl_device_uuid.data());

        u32 count = 0;
        Check(enumerate_devices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        Check(enumerate_devices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceIDProperties id{};
            id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            VkPhysicalDeviceProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &id;
            get_properties2(candidate, &properties);
            if (std::memcmp(id.deviceUUID, gl_device_uuid.data(), VK_UUID_SIZE) == 0 &&
                properties.properties.apiVersion >= VK_API_VERSION_1_2) {
                physical_device = candidate;
                break;
            }
        }
        if (!physical_device) {
            Throw("The GPU used by OpenGL wasn't found in Vulkan");
        }

        u32 extension_count = 0;
        Check(enumerate_extensions(physical_device, nullptr, &extension_count, nullptr),
              "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(extension_count);
        Check(enumerate_extensions(physical_device, nullptr, &extension_count, extensions.data()),
              "vkEnumerateDeviceExtensionProperties");
        const auto has_extension = [&extensions](const char* name) {
            return std::any_of(extensions.begin(), extensions.end(), [name](const auto& ext) {
                return std::strcmp(ext.extensionName, name) == 0;
            });
        };
        for (const char* required : {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                     VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME}) {
            if (!has_extension(required)) {
                Throw(fmt::format("The Vulkan driver lacks {}", required));
            }
        }

        VkPhysicalDeviceVulkan12Features supported12{};
        supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceFeatures2 supported{};
        supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        supported.pNext = &supported12;
        get_features2(physical_device, &supported);
        if (!supported12.timelineSemaphore) {
            Throw("The Vulkan driver lacks timeline semaphores");
        }
        float16 = supported12.shaderFloat16 == VK_TRUE;

        u32 family_count = 0;
        get_queue_families(physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        get_queue_families(physical_device, &family_count, families.data());
        std::optional<u32> compute_family;
        std::optional<u32> any_family;
        for (u32 i = 0; i < family_count; ++i) {
            const VkQueueFlags flags = families[i].queueFlags;
            if (!(flags & VK_QUEUE_COMPUTE_BIT)) {
                continue;
            }
            if (!(flags & VK_QUEUE_GRAPHICS_BIT) && !compute_family) {
                compute_family = i;
            }
            if (!any_family) {
                any_family = i;
            }
        }
        if (!any_family) {
            Throw("No Vulkan compute queue");
        }
        queue_family = compute_family.value_or(*any_family);

        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = queue_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
        VkPhysicalDeviceVulkan12Features enabled12{};
        enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        enabled12.timelineSemaphore = VK_TRUE;
        enabled12.shaderFloat16 = float16 ? VK_TRUE : VK_FALSE;
        const std::array<const char*, 2> device_extensions{
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        };
        const VkDeviceCreateInfo device_info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &enabled12,
            .flags = 0,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_info,
            .enabledLayerCount = 0,
            .ppEnabledLayerNames = nullptr,
            .enabledExtensionCount = static_cast<u32>(device_extensions.size()),
            .ppEnabledExtensionNames = device_extensions.data(),
            .pEnabledFeatures = nullptr,
        };
        Check(create_device(physical_device, &device_info, nullptr, &device), "vkCreateDevice");

        destroy_device = DeviceProc<PFN_vkDestroyDevice>("vkDestroyDevice");
        device_wait_idle = DeviceProc<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
        queue_submit = DeviceProc<PFN_vkQueueSubmit>("vkQueueSubmit");
        create_semaphore = DeviceProc<PFN_vkCreateSemaphore>("vkCreateSemaphore");
        destroy_semaphore = DeviceProc<PFN_vkDestroySemaphore>("vkDestroySemaphore");
        get_semaphore_handle =
            DeviceProc<PFN_vkGetSemaphoreWin32HandleKHR>("vkGetSemaphoreWin32HandleKHR");
        create_pool = DeviceProc<PFN_vkCreateCommandPool>("vkCreateCommandPool");
        destroy_pool = DeviceProc<PFN_vkDestroyCommandPool>("vkDestroyCommandPool");
        allocate_cmdbufs = DeviceProc<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");
        begin_cmdbuf = DeviceProc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
        end_cmdbuf = DeviceProc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
        cmd_barrier = DeviceProc<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        const auto get_queue = DeviceProc<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
        get_queue(device, queue_family, 0, &queue);

        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue_family,
        };
        Check(create_pool(device, &pool_info, nullptr, &command_pool), "vkCreateCommandPool");
    }

    SharedSemaphore CreateSharedSemaphore() {
        SharedSemaphore semaphore;
        const VkExportSemaphoreCreateInfo export_info{
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
        };
        const VkSemaphoreCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &export_info,
            .flags = 0,
        };
        Check(create_semaphore(device, &info, nullptr, &semaphore.vk), "vkCreateSemaphore");
        const VkSemaphoreGetWin32HandleInfoKHR handle_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
            .pNext = nullptr,
            .semaphore = semaphore.vk,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
        };
        HANDLE handle{};
        Check(get_semaphore_handle(device, &handle_info, &handle), "vkGetSemaphoreWin32HandleKHR");
        glGenSemaphoresEXT(1, &semaphore.gl);
        glImportSemaphoreWin32HandleEXT(semaphore.gl, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        semaphore.handle = handle;
        return semaphore;
    }

    SharedImage ImportImage(const lsfgvk::backend::ExportedImage& exported) {
        SharedImage image;
        glCreateMemoryObjectsEXT(1, &image.memory);
        const GLint dedicated = GL_TRUE;
        glMemoryObjectParameterivEXT(image.memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        glImportMemoryWin32HandleEXT(image.memory, exported.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                     exported.handle);
        image.handle = exported.handle;
        glCreateTextures(GL_TEXTURE_2D, 1, &image.texture);
        glTextureStorageMem2DEXT(image.texture, 1, GL_RGBA8,
                                 static_cast<GLsizei>(exported.width),
                                 static_cast<GLsizei>(exported.height), image.memory, 0);
        glCreateFramebuffers(1, &image.framebuffer);
        glNamedFramebufferTexture(image.framebuffer, GL_COLOR_ATTACHMENT0, image.texture, 0);
        if (glCheckNamedFramebufferStatus(image.framebuffer, GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            Throw("Shared frame generation image can't be used by OpenGL");
        }
        return image;
    }

    void RecordOwnership(VkCommandBuffer cmdbuf, bool acquire) {
        const VkCommandBufferBeginInfo begin{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
            .pInheritanceInfo = nullptr,
        };
        Check(begin_cmdbuf(cmdbuf, &begin), "vkBeginCommandBuffer");
        std::vector<VkImageMemoryBarrier> barriers;
        const auto add = [&](VkImage image) {
            barriers.push_back(VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = acquire ? VkAccessFlags{0}
                                         : VkAccessFlags{VK_ACCESS_SHADER_WRITE_BIT |
                                                         VK_ACCESS_SHADER_READ_BIT},
                .dstAccessMask = acquire ? VkAccessFlags{VK_ACCESS_SHADER_READ_BIT |
                                                         VK_ACCESS_SHADER_WRITE_BIT}
                                         : VkAccessFlags{0},
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : queue_family,
                .dstQueueFamilyIndex = acquire ? queue_family : VK_QUEUE_FAMILY_EXTERNAL,
                .image = image,
                .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            });
        };
        for (size_t i = 0; i < 2; ++i) {
            add(backend->sourceImage(*context, i));
        }
        for (u32 i = 0; i < generated; ++i) {
            add(backend->destinationImage(*context, i));
        }
        cmd_barrier(cmdbuf,
                    acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                            : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    acquire ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                            : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    0, 0, nullptr, 0, nullptr, static_cast<u32>(barriers.size()),
                    barriers.data());
        Check(end_cmdbuf(cmdbuf), "vkEndCommandBuffer");
    }

    void Submit(VkCommandBuffer cmdbuf, VkSemaphore wait_binary, VkSemaphore wait_timeline,
                u64 wait_value, VkSemaphore signal, u64 signal_value) {
        const VkSemaphore wait = wait_binary ? wait_binary : wait_timeline;
        const u64 wait_values[1]{wait_binary ? 0 : wait_value};
        const u64 signal_values[1]{signal_value};
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        const VkTimelineSemaphoreSubmitInfo timeline{
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = wait_values,
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = signal_values,
        };
        const VkSubmitInfo info{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timeline,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &wait,
            .pWaitDstStageMask = &stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmdbuf,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &signal,
        };
        Check(queue_submit(queue, 1, &info, VK_NULL_HANDLE), "vkQueueSubmit");
    }

    void CreateContext(u32 new_width, u32 new_height, u32 new_generated, int new_flow,
                       bool new_performance) {
        DestroyContext();
        const float flow = 100.0f / static_cast<float>(new_flow);
        context = &backend->openContext(new_width, new_height, new_generated, false, flow,
                                        new_performance, true);
        width = new_width;
        height = new_height;
        generated = new_generated;
        flow_scale = new_flow;
        performance = new_performance;

        for (size_t i = 0; i < 2; ++i) {
            sources[i] = ImportImage(backend->exportSourceImage(*context, i));
        }
        destinations.clear();
        for (u32 i = 0; i < generated; ++i) {
            destinations.push_back(ImportImage(backend->exportDestinationImage(*context, i)));
        }
        textures.clear();
        textures.push_back(sources[0].texture);
        textures.push_back(sources[1].texture);
        for (const auto& image : destinations) {
            textures.push_back(image.texture);
        }
        layouts.assign(textures.size(), GL_LAYOUT_GENERAL_EXT);

        gl_done = CreateSharedSemaphore();
        vk_done = CreateSharedSemaphore();

        std::array<VkCommandBuffer, 2> cmdbufs{};
        const VkCommandBufferAllocateInfo alloc{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 2,
        };
        Check(allocate_cmdbufs(device, &alloc, cmdbufs.data()), "vkAllocateCommandBuffers");
        acquire_cmdbuf = cmdbufs[0];
        release_cmdbuf = cmdbufs[1];
        RecordOwnership(acquire_cmdbuf, true);
        RecordOwnership(release_cmdbuf, false);

        const VkSubmitInfo release{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &release_cmdbuf,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &vk_done.vk,
        };
        Check(queue_submit(queue, 1, &release, VK_NULL_HANDLE), "vkQueueSubmit");
        vk_done_pending = true;
        warmup = 1;

        LOG_INFO(Render_OpenGL, "Frame generation: {}x at {}x{}, flow scale {}%{}", generated + 1,
                 width, height, flow_scale, performance ? ", performance mode" : "");
    }

    void DestroyContext() {
        if (!context) {
            return;
        }
        glFinish();
        if (device_wait_idle) {
            device_wait_idle(device);
        }
        backend->closeContext(*context);
        context = nullptr;
        for (auto& image : sources) {
            DeleteImage(image);
        }
        for (auto& image : destinations) {
            DeleteImage(image);
        }
        destinations.clear();
        textures.clear();
        DeleteSemaphore(gl_done);
        DeleteSemaphore(vk_done);
        if (acquire_cmdbuf || release_cmdbuf) {
            acquire_cmdbuf = VK_NULL_HANDLE;
            release_cmdbuf = VK_NULL_HANDLE;
        }
        vk_done_pending = false;
        width = height = generated = 0;
    }

    void DeleteImage(SharedImage& image) {
        if (image.framebuffer) {
            glDeleteFramebuffers(1, &image.framebuffer);
        }
        if (image.texture) {
            glDeleteTextures(1, &image.texture);
        }
        if (image.memory) {
            glDeleteMemoryObjectsEXT(1, &image.memory);
        }
        if (image.handle) {
            CloseHandle(static_cast<HANDLE>(image.handle));
        }
        image = {};
    }

    void DeleteSemaphore(SharedSemaphore& semaphore) {
        if (semaphore.gl) {
            glDeleteSemaphoresEXT(1, &semaphore.gl);
        }
        if (semaphore.vk) {
            destroy_semaphore(device, semaphore.vk, nullptr);
        }
        if (semaphore.handle) {
            CloseHandle(static_cast<HANDLE>(semaphore.handle));
        }
        semaphore = {};
    }

    void DestroyAll() {
        DestroyContext();
        backend.reset();
        backend_dll_setting.clear();
        if (device) {
            device_wait_idle(device);
            if (command_pool) {
                destroy_pool(device, command_pool, nullptr);
                command_pool = VK_NULL_HANDLE;
            }
            destroy_device(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance) {
            const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
                get_instance_proc(instance, "vkDestroyInstance"));
            if (destroy_instance) {
                destroy_instance(instance, nullptr);
            }
            instance = VK_NULL_HANDLE;
        }
        physical_device = VK_NULL_HANDLE;
        device_wait_idle = nullptr;
        library.reset();
    }

    std::shared_ptr<Common::DynamicLibrary> library;
    PFN_vkGetInstanceProcAddr get_instance_proc{};
    PFN_vkGetDeviceProcAddr get_device_proc{};
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    VkQueue queue{};
    u32 queue_family = 0;
    bool float16 = false;
    VkCommandPool command_pool{};
    VkCommandBuffer acquire_cmdbuf{};
    VkCommandBuffer release_cmdbuf{};
    PFN_vkDestroyDevice destroy_device{};
    PFN_vkDeviceWaitIdle device_wait_idle{};
    PFN_vkQueueSubmit queue_submit{};
    PFN_vkCreateSemaphore create_semaphore{};
    PFN_vkDestroySemaphore destroy_semaphore{};
    PFN_vkGetSemaphoreWin32HandleKHR get_semaphore_handle{};
    PFN_vkCreateCommandPool create_pool{};
    PFN_vkDestroyCommandPool destroy_pool{};
    PFN_vkAllocateCommandBuffers allocate_cmdbufs{};
    PFN_vkBeginCommandBuffer begin_cmdbuf{};
    PFN_vkEndCommandBuffer end_cmdbuf{};
    PFN_vkCmdPipelineBarrier cmd_barrier{};

    std::unique_ptr<lsfgvk::backend::Instance> backend;
    lsfgvk::backend::ContextImpl* context = nullptr;
    std::string backend_dll_setting;
    std::string failed_key;

    std::array<SharedImage, 2> sources{};
    std::vector<SharedImage> destinations;
    std::vector<GLuint> textures;
    std::vector<GLenum> layouts;
    SharedSemaphore gl_done;
    SharedSemaphore vk_done;
    bool vk_done_pending = false;

    u32 width = 0;
    u32 height = 0;
    u32 generated = 0;
    int flow_scale = 0;
    bool performance = false;
    u32 warmup = 0;
};

#else

class FrameGeneratorGL {
public:
    u32 Prepare(u32, u32) {
        FG::SetState(FG::State::Unsupported,
                     "Frame generation with OpenGL isn't available in this build.");
        return 0;
    }
    bool Submit(GLuint) {
        return false;
    }
    GLuint GeneratedFramebuffer(u32) const {
        return 0;
    }
    void Fail(const std::string& message) {
        FG::SetState(FG::State::Failed, message);
    }
};

#endif

FramePresenter::FramePresenter(Core::Frontend::EmuWindow& emu_window_)
    : emu_window{emu_window_}, context{emu_window.CreateSharedContext()} {
    for (std::size_t i = 0; i < SLOT_COUNT; ++i) {
        free_slots.push_back(i);
    }
    thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
}

FramePresenter::~FramePresenter() {
    thread.request_stop();
    present_cv.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
    for (auto& slot : slots) {
        if (slot.ready) {
            glDeleteSync(slot.ready);
        }
        if (slot.released) {
            glDeleteSync(slot.released);
        }
    }
}

GLuint FramePresenter::BeginFrame(u32 width, u32 height) {
    {
        std::unique_lock lock{mutex};
        free_cv.wait(lock, [this] { return !free_slots.empty(); });
        current_slot = free_slots.front();
        free_slots.pop_front();
    }
    Slot& slot = slots[current_slot];
    if (slot.released) {
        glWaitSync(slot.released, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(slot.released);
        slot.released = {};
    }
    if (slot.width != width || slot.height != height || slot.texture.handle == 0) {
        slot.texture.Release();
        slot.framebuffer.Release();
        slot.texture.Create(GL_TEXTURE_2D);
        glTextureStorage2D(slot.texture.handle, 1, GL_RGBA8, static_cast<GLsizei>(width),
                           static_cast<GLsizei>(height));
        slot.framebuffer.Create();
        glNamedFramebufferTexture(slot.framebuffer.handle, GL_COLOR_ATTACHMENT0,
                                  slot.texture.handle, 0);
        slot.width = width;
        slot.height = height;
        ++slot.generation;
    }
    frame_open = true;
    return slot.framebuffer.handle;
}

void FramePresenter::EndFrame() {
    if (!frame_open) {
        return;
    }
    frame_open = false;
    Slot& slot = slots[current_slot];
    slot.ready = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    {
        std::scoped_lock lock{mutex};
        present_queue.push_back(current_slot);
    }
    present_cv.notify_all();
}

void FramePresenter::Drain() {
    std::unique_lock lock{mutex};
    free_cv.wait(lock, [this] { return present_queue.empty() && !busy; });
}

void FramePresenter::ReleaseGenerator() {
    {
        std::scoped_lock lock{mutex};
        release_requested = true;
    }
    present_cv.notify_all();
}

bool FramePresenter::NewerFrameWaiting() {
    std::scoped_lock lock{mutex};
    return !present_queue.empty();
}

void FramePresenter::WaitForPresentTime(std::chrono::steady_clock::time_point target) {
    using namespace std::chrono_literals;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= target || NewerFrameWaiting()) {
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

void FramePresenter::PresentFramebuffer(GLuint framebuffer, u32 width, u32 height) {
    const GLint w = static_cast<GLint>(width);
    const GLint h = static_cast<GLint>(height);
    glBlitNamedFramebuffer(framebuffer, 0, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                           GL_NEAREST);
    context->SwapBuffers();
}

void FramePresenter::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("OpenGLPresent");
    context->MakeCurrent();
    while (!token.stop_requested()) {
        std::size_t index;
        {
            std::unique_lock lock{mutex};
            present_cv.wait(lock, token,
                            [this] { return !present_queue.empty() || release_requested; });
            if (token.stop_requested()) {
                break;
            }
            if (release_requested && present_queue.empty()) {
                release_requested = false;
                lock.unlock();
                generator.reset();
                continue;
            }
            index = present_queue.front();
            present_queue.pop_front();
            busy = true;
        }
        Slot& slot = slots[index];
        try {
            PresentSlot(slot);
        } catch (const std::exception& e) {
            LOG_ERROR(Render_OpenGL, "Presentation failed: {}", e.what());
        }
        slot.released = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        {
            std::scoped_lock lock{mutex};
            free_slots.push_back(index);
            busy = false;
        }
        free_cv.notify_all();
    }
    generator.reset();
    for (auto& slot : slots) {
        if (slot.present_framebuffer) {
            glDeleteFramebuffers(1, &slot.present_framebuffer);
            slot.present_framebuffer = 0;
        }
    }
    context->DoneCurrent();
}

void FramePresenter::PresentSlot(Slot& slot) {
    using Clock = std::chrono::steady_clock;

    glWaitSync(slot.ready, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(slot.ready);
    slot.ready = {};

    if (slot.present_generation != slot.generation || slot.present_framebuffer == 0) {
        if (slot.present_framebuffer) {
            glDeleteFramebuffers(1, &slot.present_framebuffer);
        }
        glCreateFramebuffers(1, &slot.present_framebuffer);
        glNamedFramebufferTexture(slot.present_framebuffer, GL_COLOR_ATTACHMENT0,
                                  slot.texture.handle, 0);
        slot.present_generation = slot.generation;
    }

    u32 generated = 0;
    if (FG::Enabled()) {
        if (!generator) {
            generator = std::make_unique<FrameGeneratorGL>();
        }
        generated = generator->Prepare(slot.width, slot.height);
    } else if (generator) {
        generator.reset();
    }

    u32 shown = 0;
    if (generated > 0) {
        bool usable = false;
        try {
            usable = generator->Submit(slot.present_framebuffer);
        } catch (const std::exception& e) {
            generator->Fail(e.what());
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

        if (usable) {
            for (u32 i = 0; i < generated; ++i) {
                if (i > 0) {
                    WaitForPresentTime(present_time);
                }
                if (NewerFrameWaiting()) {
                    break;
                }
                PresentFramebuffer(generator->GeneratedFramebuffer(i), slot.width, slot.height);
                present_time += step;
                ++shown;
            }
            WaitForPresentTime(present_time);
        }
    } else {
        last_frame_time = {};
    }

    PresentFramebuffer(slot.present_framebuffer, slot.width, slot.height);
    if (generated > 0) {
        FG::CountPresented(1, shown);
    }
}

} // namespace OpenGL
