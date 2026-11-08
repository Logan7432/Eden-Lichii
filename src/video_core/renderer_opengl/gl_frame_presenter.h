// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <glad/glad.h>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"

namespace Core::Frontend {
class EmuWindow;
class GraphicsContext;
} // namespace Core::Frontend

namespace OpenGL {

class FrameGeneratorGL;

class FramePresenter {
public:
    explicit FramePresenter(Core::Frontend::EmuWindow& emu_window);
    ~FramePresenter();

    FramePresenter(const FramePresenter&) = delete;
    FramePresenter& operator=(const FramePresenter&) = delete;

    GLuint BeginFrame(u32 width, u32 height);

    void EndFrame();

    void Drain();

    void ReleaseGenerator();

private:
    static constexpr std::size_t SLOT_COUNT = 3;

    struct Slot {
        OGLTexture texture;
        OGLFramebuffer framebuffer;
        u32 width = 0;
        u32 height = 0;
        u32 generation = 0;
        GLsync ready{};
        GLsync released{};
        GLuint present_framebuffer = 0;
        u32 present_generation = 0;
    };

    void PresentThread(std::stop_token token);
    void PresentSlot(Slot& slot);
    void PresentFramebuffer(GLuint framebuffer, u32 width, u32 height);
    bool NewerFrameWaiting();
    void WaitForPresentTime(std::chrono::steady_clock::time_point target);

    Core::Frontend::EmuWindow& emu_window;
    std::unique_ptr<Core::Frontend::GraphicsContext> context;

    std::array<Slot, SLOT_COUNT> slots{};
    std::size_t current_slot = 0;
    bool frame_open = false;

    std::mutex mutex;
    std::condition_variable_any present_cv;
    std::condition_variable free_cv;
    std::deque<std::size_t> present_queue;
    std::deque<std::size_t> free_slots;
    bool busy = false;
    bool release_requested = false;

    std::unique_ptr<FrameGeneratorGL> generator;
    std::chrono::steady_clock::time_point last_frame_time{};
    std::chrono::nanoseconds frame_interval{16'666'667};

    std::jthread thread;
};

} // namespace OpenGL
