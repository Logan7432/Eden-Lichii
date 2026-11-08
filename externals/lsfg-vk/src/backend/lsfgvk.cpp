/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "extraction/dll_reader.hpp"
#include "extraction/shader_registry.hpp"
#include "helpers/limits.hpp"
#include "helpers/utils.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/buffer.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/timeline_semaphore.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "shaderchains/alpha0.hpp"
#include "shaderchains/alpha1.hpp"
#include "shaderchains/beta0.hpp"
#include "shaderchains/beta1.hpp"
#include "shaderchains/delta0.hpp"
#include "shaderchains/delta1.hpp"
#include "shaderchains/gamma0.hpp"
#include "shaderchains/gamma1.hpp"
#include "shaderchains/generate.hpp"
#include "shaderchains/mipmaps.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>


using namespace lsfgvk;
using namespace lsfgvk::backend;

namespace lsfgvk::backend {
    error::error(const std::string& msg, const std::exception& inner)
        : std::runtime_error(msg + "\n- " + inner.what()) {}
    error::error(const std::string& msg)
        : std::runtime_error(msg) {}
    error::~error() = default;

    /// instance class
    class InstanceImpl {
    public:
        /// create an instance
        /// (see lsfg-vk documentation)
        InstanceImpl(const HostDevice& host,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision,
            const std::optional<std::filesystem::path>& pipelineCache);

        /// get the Vulkan instance
        /// @return the Vulkan instance
        [[nodiscard]] const auto& getVulkan() const { return this->vk; }
        /// get the shader registry
        /// @return the shader registry
        [[nodiscard]] const auto& getShaderRegistry() const { return this->shaders; }

        // Non-copyable, non-movable
        InstanceImpl(const InstanceImpl&) = delete;
        InstanceImpl& operator=(const InstanceImpl&) = delete;
        InstanceImpl(InstanceImpl&&) = delete;
        InstanceImpl& operator=(InstanceImpl&&) = delete;
        ~InstanceImpl() = default;
    private:
        vk::Vulkan vk;
        ShaderRegistry shaders;
    };

    /// context class
    class ContextImpl {
    public:
        /// create a context
        /// (see lsfg-vk documentation)
        ContextImpl(const InstanceImpl& instance,
            VkExtent2D extent, uint32_t generated, bool hdr, float flow, bool perf,
            bool exportable);

        /// schedule frames
        /// (see lsfg-vk documentation)
        void scheduleFrames();

        /// work the host has to do for the next real frame
        [[nodiscard]] FrameTicket nextFrame() const;

        [[nodiscard]] VkImage destinationImage(size_t i) const {
            return this->destImages.at(i).handle();
        }
        [[nodiscard]] VkSemaphore syncSemaphoreHandle() const {
            return this->syncSemaphore.handle();
        }
        [[nodiscard]] const vk::Image& source(size_t i) const {
            return i == 0 ? this->sourceImages.first : this->sourceImages.second;
        }
        [[nodiscard]] const vk::Image& destination(size_t i) const {
            return this->destImages.at(i);
        }
        [[nodiscard]] const vk::Vulkan& vulkan() const {
            return this->ctx.vk.get();
        }
    private:
        std::pair<vk::Image, vk::Image> sourceImages;
        std::vector<vk::Image> destImages;
        vk::Image blackImage;

        vk::TimelineSemaphore syncSemaphore; // shared with the host
        vk::TimelineSemaphore prepassSemaphore;
        size_t idx{1};
        size_t fidx{0}; // real frame index

        std::vector<vk::CommandBuffer> cmdbufs;
        vk::Fence cmdbufFence;

        Ctx ctx;

        Mipmaps mipmaps;
        std::array<Alpha0, 7> alpha0;
        std::array<Alpha1, 7> alpha1;
        Beta0 beta0;
        Beta1 beta1;
        struct Pass {
            std::vector<Gamma0> gamma0;
            std::vector<Gamma1> gamma1;

            std::vector<Delta0> delta0;
            std::vector<Delta1> delta1;
            ls::lazy<Generate> generate;
        };
        std::vector<Pass> passes;
    };
}

Instance::Instance(
        const HostDevice& host,
        const std::filesystem::path& shaderDllPath,
        bool allowLowPrecision,
        const std::optional<std::filesystem::path>& pipelineCache) {
    this->m_impl = std::make_unique<InstanceImpl>(
        host, shaderDllPath, allowLowPrecision, pipelineCache
    );
}

namespace {
    /// wrap the host's Vulkan device
    vk::Vulkan createVulkan(const HostDevice& host,
            const std::optional<std::filesystem::path>& pipelineCache) {
        try {
            return{
                host.instance, host.physicalDevice, host.device,
                host.getInstanceProcAddr, host.queue, host.queueFamily, host.float16,
                host.queueLock, pipelineCache
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to initialize Vulkan", e);
        }
    }
    /// build a shader registry
    ShaderRegistry createShaderRegistry(vk::Vulkan& vk,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision) {
        std::unordered_map<uint32_t, std::vector<uint8_t>> resources{};

        try {
            resources = backend::extractResourcesFromDLL(shaderDllPath);
        } catch (const std::exception& e) {
            throw backend::error("Unable to parse Lossless Scaling DLL", e);
        }

        try {
            return backend::buildShaderRegistry(
                vk, allowLowPrecision && vk.supportsFP16(),
                resources
            );
        } catch (const std::exception& e) {
            throw backend::error("Unable to build shader registry", e);
        }
    }
}

InstanceImpl::InstanceImpl(const HostDevice& host,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision,
            const std::optional<std::filesystem::path>& pipelineCache)
        : vk(createVulkan(host, pipelineCache)),
        shaders(createShaderRegistry(this->vk, shaderDllPath, allowLowPrecision)) {
    vk.persistPipelineCache(); // will silently fail
}

Context& Instance::openContext(uint32_t width, uint32_t height, uint32_t generated,
        bool hdr, float flow, bool perf, bool exportable) {
    const VkExtent2D extent{ width, height };
    return *this->m_contexts.emplace_back(std::make_unique<ContextImpl>(*this->m_impl,
        extent, generated, hdr, flow, perf, exportable
    )).get();
}

VkImage Instance::sourceImage(const Context& context, size_t i) const {
    return context.source(i).handle();
}

namespace {
    ExportedImage exportImage(const vk::Vulkan& vk, const vk::Image& image) {
        try {
            return ExportedImage{
                .handle = image.exportWin32Handle(vk),
                .size = image.allocationSize(),
                .width = image.getExtent().width,
                .height = image.getExtent().height,
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to export image", e);
        }
    }
}

ExportedImage Instance::exportSourceImage(const Context& context, size_t i) const {
    return exportImage(context.vulkan(), context.source(i));
}

ExportedImage Instance::exportDestinationImage(const Context& context, size_t i) const {
    return exportImage(context.vulkan(), context.destination(i));
}

FrameTicket Instance::nextFrame(const Context& context) const {
    return context.nextFrame();
}

VkImage Instance::destinationImage(const Context& context, size_t i) const {
    return context.destinationImage(i);
}

VkSemaphore Instance::syncSemaphore(const Context& context) const {
    return context.syncSemaphoreHandle();
}

namespace {
    /// create the two source images the host copies real frames into
    std::pair<vk::Image, vk::Image> createSourceImages(const vk::Vulkan& vk,
            VkExtent2D extent, VkFormat format, bool exportable) {
        try {
            // Shared images also get color attachment usage: the other API renders into them.
            const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                (exportable ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
            return {
                vk::Image(vk, extent, format, usage, exportable),
                vk::Image(vk, extent, format, usage, exportable)
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create source images", e);
        }
    }
    /// create the destination images the generated frames are written to
    std::vector<vk::Image> createDestinationImages(const vk::Vulkan& vk,
            uint32_t count, VkExtent2D extent, VkFormat format, bool exportable) {
        try {
            std::vector<vk::Image> destImages;
            destImages.reserve(count);

            for (uint32_t i = 0; i < count; ++i)
                destImages.emplace_back(vk, extent, format,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    (exportable ? VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
                    exportable);

            return destImages;
        } catch (const std::exception& e) {
            throw backend::error("Unable to create destination images", e);
        }
    }
    /// create a black image
    vk::Image createBlackImage(const vk::Vulkan& vk) {
        try {
            return{vk,
                { .width = 4, .height = 4 }
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create black image", e);
        }
    }
    /// create the timeline semaphore shared with the host
    vk::TimelineSemaphore createSyncSemaphore(const vk::Vulkan& vk) {
        try {
            return{vk, 0};
        } catch (const std::exception& e) {
            throw backend::error("Unable to create sync semaphore", e);
        }
    }
    /// create prepass semaphores
    vk::TimelineSemaphore createPrepassSemaphore(const vk::Vulkan& vk) {
        try {
            return{vk, 0};
        } catch (const std::exception& e) {
            throw backend::error("Unable to create prepass semaphore", e);
        }
    }
    /// create command buffers
    std::vector<vk::CommandBuffer> createCommandBuffers(const vk::Vulkan& vk, size_t count) {
        try {
            std::vector<vk::CommandBuffer> cmdbufs;
            cmdbufs.reserve(count);

            for (size_t i = 0; i < count; ++i)
                cmdbufs.emplace_back(vk);

            return cmdbufs;
        } catch (const std::exception& e) {
            throw backend::error("Unable to create command buffers", e);
        }
    }
    /// create context data
    Ctx createCtx(const InstanceImpl& instance, VkExtent2D extent,
            bool hdr, float flow, bool perf, size_t count) {
        const auto& vk = instance.getVulkan();
        const auto& shaders = instance.getShaderRegistry();

        try {
            std::vector<vk::Buffer> constantBuffers{};
            constantBuffers.reserve(count);

            for (size_t i = 0; i < count; ++i)
                constantBuffers.emplace_back(vk,
                    backend::getDefaultConstantBuffer(
                        i, count, flow
                    )
                );

            return {
                .vk = std::ref(vk),
                .shaders = std::ref(shaders),
                .pool{vk, backend::calculateDescriptorPoolLimits(count, perf)},
                .constantBuffer{vk, backend::getDefaultConstantBuffer(0, 1, flow)},
                .constantBuffers{std::move(constantBuffers)},
                .bnbSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, false},
                .bnwSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, true},
                .eabSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, false},
                .sourceExtent = extent,
                .flowExtent = VkExtent2D {
                    .width = static_cast<uint32_t>(static_cast<float>(extent.width) / flow),
                    .height = static_cast<uint32_t>(static_cast<float>(extent.height) / flow)
                },
                .hdr = hdr,
                .flow = flow,
                .perf = perf,
                .count = count
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create context", e);
        }
    }
}

ContextImpl::ContextImpl(const InstanceImpl& instance,
            VkExtent2D extent, uint32_t generated, bool hdr, float flow, bool perf,
            bool exportable) :
        sourceImages(createSourceImages(instance.getVulkan(),
            extent, hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM, exportable)),
        destImages(createDestinationImages(instance.getVulkan(), generated,
            extent, hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM, exportable)),
        blackImage(createBlackImage(instance.getVulkan())),
        syncSemaphore(createSyncSemaphore(instance.getVulkan())),
        prepassSemaphore(createPrepassSemaphore(instance.getVulkan())),
        cmdbufs(createCommandBuffers(instance.getVulkan(), generated + 1)),
        cmdbufFence(instance.getVulkan()),
        ctx(createCtx(instance, extent, hdr, flow, perf, generated)),
        mipmaps(ctx, sourceImages),
        alpha0{
            Alpha0(ctx, mipmaps.getImages().at(0)),
            Alpha0(ctx, mipmaps.getImages().at(1)),
            Alpha0(ctx, mipmaps.getImages().at(2)),
            Alpha0(ctx, mipmaps.getImages().at(3)),
            Alpha0(ctx, mipmaps.getImages().at(4)),
            Alpha0(ctx, mipmaps.getImages().at(5)),
            Alpha0(ctx, mipmaps.getImages().at(6))
        },
        alpha1{
            Alpha1(ctx, 3, alpha0.at(0).getImages()),
            Alpha1(ctx, 2, alpha0.at(1).getImages()),
            Alpha1(ctx, 2, alpha0.at(2).getImages()),
            Alpha1(ctx, 2, alpha0.at(3).getImages()),
            Alpha1(ctx, 2, alpha0.at(4).getImages()),
            Alpha1(ctx, 2, alpha0.at(5).getImages()),
            Alpha1(ctx, 2, alpha0.at(6).getImages())
        },
        beta0(ctx, alpha1.at(0).getImages()),
        beta1(ctx, beta0.getImages()) {
    // build main passes
    for (size_t i = 0; i < destImages.size(); ++i) {
        auto& pass = this->passes.emplace_back();

        pass.gamma0.reserve(7);
        pass.gamma1.reserve(7);
        pass.delta0.reserve(3);
        pass.delta1.reserve(3);
        for (size_t j = 0; j < 7; j++) {
            if (j == 0) { // first pass has no prior data
                pass.gamma0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    this->blackImage
                );
                pass.gamma1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    this->blackImage,
                    this->beta1.getImages().at(5)
                );
            } else { // other passes use prior data
                pass.gamma0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.gamma1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    pass.gamma1.at(j - 1).getImage(),
                    this->beta1.getImages().at(6 - j)
                );
            }

            if (j == 4) { // first special pass has no prior data
                pass.delta0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    this->blackImage,
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.delta1.emplace_back(ctx, i,
                    pass.delta0.at(j - 4).getImages0(),
                    pass.delta0.at(j - 4).getImages1(),
                    this->blackImage,
                    this->beta1.getImages().at(6 - j),
                    this->blackImage
                );
            } else if (j > 4) { // further passes do
                pass.delta0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    pass.delta1.at(j - 5).getImage0(),
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.delta1.emplace_back(ctx, i,
                    pass.delta0.at(j - 4).getImages0(),
                    pass.delta0.at(j - 4).getImages1(),
                    pass.delta1.at(j - 5).getImage0(),
                    this->beta1.getImages().at(6 - j),
                    pass.delta1.at(j - 5).getImage1()
                );
            }
        }

        pass.generate.emplace(ctx, i,
            this->sourceImages,
            pass.gamma1.at(6).getImage(),
            pass.delta1.at(2).getImage0(),
            pass.delta1.at(2).getImage1(),
            this->destImages.at(i)
        );
    }

    // initialize all images
    std::vector<VkImage> images{};
    images.push_back(this->blackImage.handle());
    images.push_back(this->sourceImages.first.handle());
    images.push_back(this->sourceImages.second.handle());
    for (const auto& image : this->destImages)
        images.push_back(image.handle());
    mipmaps.prepare(images);
    for (size_t i = 0; i < 7; ++i) {
        alpha0.at(i).prepare(images);
        alpha1.at(i).prepare(images);
    }
    beta0.prepare(images);
    beta1.prepare(images);
    for (const auto& pass : this->passes) {
        for (size_t i = 0; i < 7; ++i) {
            pass.gamma0.at(i).prepare(images);
            pass.gamma1.at(i).prepare(images);

            if (i < 4) continue;
            pass.delta0.at(i - 4).prepare(images);
            pass.delta1.at(i - 4).prepare(images);
        }
    }

    std::vector<vk::Barrier> barriers{};
    barriers.reserve(images.size());

    for (const auto& image : images) {
        barriers.emplace_back(vk::Barrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1
            }
        });
    }

    const vk::CommandBuffer cmdbuf{ctx.vk};
    cmdbuf.begin(ctx.vk);
    cmdbuf.insertBarriers(ctx.vk, barriers);
    cmdbuf.clearImages(ctx.vk, {
        this->sourceImages.first.handle(), this->sourceImages.second.handle()
    });
    cmdbuf.end(ctx.vk);
    cmdbuf.submit(ctx.vk); // wait for completion
}

FrameTicket ContextImpl::nextFrame() const {
    FrameTicket ticket{
        .source = (this->fidx % 2 == 0) ?
            this->sourceImages.first.handle() : this->sourceImages.second.handle(),
        .sourceIndex = static_cast<uint32_t>(this->fidx % 2),
        .sourceReady = this->idx,
    };
    ticket.generatedReady.reserve(this->destImages.size());
    for (size_t i = 0; i < this->destImages.size(); i++)
        ticket.generatedReady.push_back(this->idx + 1 + i);
    return ticket;
}

void Instance::scheduleFrames(Context& context) {
    try {
        context.scheduleFrames();
    } catch (const std::exception& e) {
        throw backend::error("Unable to schedule frames", e);
    }
}

void Context::scheduleFrames() {
    // wait for previous pre-pass to complete
    if (this->fidx && !this->cmdbufFence.wait(this->ctx.vk))
        throw backend::error("Timeout waiting for previous frame to complete");
    this->cmdbufFence.reset(this->ctx.vk);

    // schedule pre-pass
    const auto& cmdbuf = this->cmdbufs.at(0);
    cmdbuf.begin(ctx.vk);

    this->mipmaps.render(ctx.vk, cmdbuf, this->fidx);
    for (size_t i = 0; i < 7; ++i) {
        this->alpha0.at(6 - i).render(ctx.vk, cmdbuf);
        this->alpha1.at(6 - i).render(ctx.vk, cmdbuf, this->fidx);
    }
    this->beta0.render(ctx.vk, cmdbuf, this->fidx);
    this->beta1.render(ctx.vk, cmdbuf);

    cmdbuf.end(ctx.vk);
    cmdbuf.submit(this->ctx.vk,
        {}, this->syncSemaphore.handle(), this->idx,
        {}, this->prepassSemaphore.handle(), this->idx
    );

    this->idx++;

    // schedule main passes
    for (size_t i = 0; i < this->destImages.size(); i++) {
        const auto& cmdbuf = this->cmdbufs.at(i + 1);
        cmdbuf.begin(ctx.vk);

        const auto& pass = this->passes.at(i);
        for (size_t j = 0; j < 7; j++) {
            pass.gamma0.at(j).render(ctx.vk, cmdbuf, this->fidx);
            pass.gamma1.at(j).render(ctx.vk, cmdbuf);

            if (j < 4) continue;
            pass.delta0.at(j - 4).render(ctx.vk, cmdbuf, this->fidx);
            pass.delta1.at(j - 4).render(ctx.vk, cmdbuf);
        }
        pass.generate->render(ctx.vk, cmdbuf, this->fidx);

        cmdbuf.end(ctx.vk);
        cmdbuf.submit(this->ctx.vk,
            {}, this->prepassSemaphore.handle(), this->idx - 1,
            {}, this->syncSemaphore.handle(), this->idx + i,
            i == this->destImages.size() - 1 ? this->cmdbufFence.handle() : VK_NULL_HANDLE
        );
    }

    this->idx += this->destImages.size();
    this->fidx++;
}

void Instance::closeContext(const Context& context) {
    auto it = std::ranges::find_if(this->m_contexts,
        [context = &context](const std::unique_ptr<ContextImpl>& ctx) {
            return ctx.get() == context;
        });
    if (it == this->m_contexts.end())
        throw backend::error("attempted to close unknown context",
            std::runtime_error("no such context"));

    this->m_impl->getVulkan().waitIdle();

    this->m_contexts.erase(it);
}

Instance::~Instance() {
    if (!this->m_contexts.empty()) {
        this->m_impl->getVulkan().waitIdle();
        this->m_contexts.clear();
    }
}
