# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# lsfg-vk (https://lsfg-vk.dev, GPL-3.0-or-later, see LICENSE.md): the frame generation of
# Lossless Scaling reimplemented in Vulkan. Vendored from the v2.0.0-dev backend and trimmed to
# run inside Eden on the renderer's own Vulkan device. The shaders themselves are loaded at
# runtime from the user's own copy of Lossless Scaling (Lossless.dll); none are shipped.

add_library(lsfg-vk STATIC
    include/lsfg-vk-backend/lsfgvk.hpp
    include/lsfg-vk-common/helpers/errors.hpp
    include/lsfg-vk-common/helpers/pointers.hpp
    include/lsfg-vk-common/vulkan/buffer.hpp
    include/lsfg-vk-common/vulkan/command_buffer.hpp
    include/lsfg-vk-common/vulkan/descriptor_pool.hpp
    include/lsfg-vk-common/vulkan/descriptor_set.hpp
    include/lsfg-vk-common/vulkan/fence.hpp
    include/lsfg-vk-common/vulkan/image.hpp
    include/lsfg-vk-common/vulkan/sampler.hpp
    include/lsfg-vk-common/vulkan/shader.hpp
    include/lsfg-vk-common/vulkan/timeline_semaphore.hpp
    include/lsfg-vk-common/vulkan/vulkan.hpp
    src/common/helpers/errors.cpp
    src/common/vulkan/buffer.cpp
    src/common/vulkan/command_buffer.cpp
    src/common/vulkan/descriptor_pool.cpp
    src/common/vulkan/descriptor_set.cpp
    src/common/vulkan/fence.cpp
    src/common/vulkan/image.cpp
    src/common/vulkan/sampler.cpp
    src/common/vulkan/shader.cpp
    src/common/vulkan/timeline_semaphore.cpp
    src/common/vulkan/vulkan.cpp
    src/backend/extraction/dll_reader.cpp
    src/backend/extraction/dll_reader.hpp
    src/backend/extraction/shader_registry.cpp
    src/backend/extraction/shader_registry.hpp
    src/backend/helpers/limits.cpp
    src/backend/helpers/limits.hpp
    src/backend/helpers/managed_shader.cpp
    src/backend/helpers/managed_shader.hpp
    src/backend/helpers/utils.cpp
    src/backend/helpers/utils.hpp
    src/backend/shaderchains/alpha0.cpp
    src/backend/shaderchains/alpha0.hpp
    src/backend/shaderchains/alpha1.cpp
    src/backend/shaderchains/alpha1.hpp
    src/backend/shaderchains/beta0.cpp
    src/backend/shaderchains/beta0.hpp
    src/backend/shaderchains/beta1.cpp
    src/backend/shaderchains/beta1.hpp
    src/backend/shaderchains/delta0.cpp
    src/backend/shaderchains/delta0.hpp
    src/backend/shaderchains/delta1.cpp
    src/backend/shaderchains/delta1.hpp
    src/backend/shaderchains/gamma0.cpp
    src/backend/shaderchains/gamma0.hpp
    src/backend/shaderchains/gamma1.cpp
    src/backend/shaderchains/gamma1.hpp
    src/backend/shaderchains/generate.cpp
    src/backend/shaderchains/generate.hpp
    src/backend/shaderchains/mipmaps.cpp
    src/backend/shaderchains/mipmaps.hpp
    src/backend/lsfgvk.cpp
)

target_include_directories(lsfg-vk PUBLIC include PRIVATE src/backend)
target_link_libraries(lsfg-vk PUBLIC Vulkan::Headers)
target_compile_features(lsfg-vk PUBLIC cxx_std_20)
# Only function pointers are used (the host's loader provides them)
target_compile_definitions(lsfg-vk PRIVATE VK_NO_PROTOTYPES)

if (MSVC)
    target_compile_options(lsfg-vk PRIVATE /W3 /permissive- /Zc:__cplusplus /utf-8)
endif()
