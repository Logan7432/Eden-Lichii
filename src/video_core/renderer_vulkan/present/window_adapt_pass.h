// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <list>

#include "common/math_util.h"
#include "common/overlay_panels.h"
#include "video_core/renderer_vulkan/present/hdr_convert.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Tegra {
struct FramebufferConfig;
}

namespace Vulkan {

class Device;
class MemoryAllocator;
struct Frame;
class Layer;
class Scheduler;
class RasterizerVulkan;

class WindowAdaptPass final {
public:
    explicit WindowAdaptPass(const Device& device, MemoryAllocator& memory_allocator,
                             VkFormat frame_format, vk::Sampler&& sampler,
                             vk::ShaderModule&& fragment_shader);
    ~WindowAdaptPass();

    void Draw(const Device& device, RasterizerVulkan& rasterizer, Scheduler& scheduler, size_t image_index,
              std::list<Layer>& layers, std::span<const Tegra::FramebufferConfig> configs,
              const Layout::FramebufferLayout& layout, VkFramebuffer framebuffer,
              VkExtent2D extent, const HdrPresent& output, bool overlay_panels = false);

    VkDescriptorSetLayout GetDescriptorSetLayout();
    VkRenderPass GetRenderPass();

private:
    void CreateDescriptorSetLayout(const Device& device);
    void CreatePipelineLayout(const Device& device);
    void CreateVertexShader(const Device& device);
    void CreateRenderPass(const Device& device, VkFormat frame_format);
    void CreatePipelines(const Device& device);

    MemoryAllocator& memory_allocator;

    vk::DescriptorSetLayout descriptor_set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::Sampler sampler;
    vk::ShaderModule vertex_shader;
    vk::ShaderModule fragment_shader;
    vk::RenderPass render_pass;
    vk::Pipeline opaque_pipeline;
    vk::Pipeline premultiplied_pipeline;
    vk::Pipeline coverage_pipeline;

    // The overlay panels' own pipeline: the plain present shader, pixel for pixel (the scaling
    // filter's shader would blur their text)
    vk::ShaderModule overlay_shader;
    vk::Sampler overlay_sampler;
    vk::Pipeline overlay_pipeline;

    // The overlay panels (common/overlay_panels.h): a straight-alpha quad each, composited on
    // top of the layers, for both the accelerated and the unaccelerated present paths.
    struct OverlayQuad {
        vk::Image image;
        vk::ImageView view;
        vk::DescriptorPool pool;
        vk::DescriptorSets sets;
        VkExtent2D extent{};
        std::uint64_t version = 0;
        bool ready = false;
        /// Staging buffers, used in turn: each is written again only once the GPU is past its
        /// last copy (normally no wait)
        std::array<vk::Buffer, 2> staging;
        std::array<std::size_t, 2> staging_size{};
        std::array<std::uint64_t, 2> staging_tick{};
        std::size_t next_staging = 0;
        /// The GPU tick that last drew the image: replaced only once the GPU is past it
        std::uint64_t used_tick = 0;
    };
    std::array<OverlayQuad, Common::OverlayPanelCount> overlays;
};

} // namespace Vulkan
