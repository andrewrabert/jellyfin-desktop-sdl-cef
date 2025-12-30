#pragma once

#include "egl_context.h"
#include "cef_client.h"  // For AcceleratedPaintInfo
#include <mutex>
#include <chrono>
#include <libdrm/drm_fourcc.h>

class OpenGLCompositor {
public:
    OpenGLCompositor();
    ~OpenGLCompositor();

    bool init(EGLContext_* egl, uint32_t width, uint32_t height);
    void cleanup();

    // Update overlay texture from CEF buffer (BGRA) - software path
    void updateOverlay(const void* data, int width, int height);

    // Get direct pointer to staging buffer for zero-copy writes
    void* getStagingBuffer(int width, int height);
    void markStagingDirty() { staging_pending_ = true; }
    bool hasPendingContent() const { return staging_pending_; }

    // Flush pending overlay data to GPU
    bool flushOverlay();

    // Update overlay from DMA-BUF - hardware accelerated path
    bool updateOverlayFromDmaBuf(const AcceleratedPaintInfo& info);

    // Composite overlay to screen with alpha blending
    void composite(uint32_t width, uint32_t height, float alpha);

    // Resize resources
    void resize(uint32_t width, uint32_t height);

    // Check if we have valid content to composite
    bool hasValidOverlay() const { return has_content_; }

private:
    bool createTexture();
    bool createShader();
    void destroyTexture();

    EGLContext_* egl_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Overlay texture
    GLuint texture_ = 0;
    bool has_content_ = false;

    // Staging buffer for software path
    std::vector<uint8_t> staging_buffer_;
    bool staging_pending_ = false;

    // DMA-BUF support
    bool dmabuf_supported_ = true;

    // Thread safety
    std::mutex mutex_;

    // Skip DMA-BUF imports briefly after resize
    std::chrono::steady_clock::time_point last_resize_time_;

    // Shader program
    GLuint program_ = 0;
    GLint alpha_loc_ = -1;

    // VAO for fullscreen quad
    GLuint vao_ = 0;
};
