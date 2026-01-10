#pragma once

#ifdef __APPLE__
#include "cgl_context.h"
#include <OpenGL/gl3.h>
#include <IOSurface/IOSurface.h>
typedef CGLContext GLContext;
#else
#include "egl_context.h"
#include <libdrm/drm_fourcc.h>
typedef EGLContext_ GLContext;
#endif

#include "cef_client.h"  // For AcceleratedPaintInfo
#include <mutex>
#include <vector>
#include <chrono>

class OpenGLCompositor {
public:
    OpenGLCompositor();
    ~OpenGLCompositor();

    bool init(GLContext* ctx, uint32_t width, uint32_t height, bool use_dmabuf = false);
    void cleanup();

    // Update overlay texture from CEF buffer (BGRA) - software path
    void updateOverlay(const void* data, int width, int height);

    // Get direct pointer to staging buffer for zero-copy writes
    void* getStagingBuffer(int width, int height);
    void markStagingDirty() { staging_pending_ = true; }
    bool hasPendingContent() const { return staging_pending_; }

    // Flush pending overlay data to GPU
    bool flushOverlay();

#ifdef __APPLE__
    // Queue IOSurface for import (thread-safe, call from any thread)
    void queueIOSurface(IOSurfaceRef surface, int width, int height);

    // Import queued IOSurface (must call from GL thread)
    bool importQueuedIOSurface();

    // Check if there's a pending IOSurface to import
    bool hasPendingIOSurface() const { return iosurface_queued_; }
#else
    // Queue DMA-BUF for import (thread-safe, call from any thread)
    void queueDmaBuf(const AcceleratedPaintInfo& info);

    // Import queued DMA-BUF (must call from GL thread)
    bool importQueuedDmaBuf();

    // Check if there's a pending DMA-BUF to import
    bool hasPendingDmaBuf() const { return !pending_dmabufs_.empty(); }
#endif

    // Composite overlay to screen with alpha blending
    void composite(uint32_t width, uint32_t height, float alpha);

    // Resize resources
    void resize(uint32_t width, uint32_t height);

    // Set visibility (no-op on Linux, alpha controls rendering)
    void setVisible(bool visible) { (void)visible; }

    // Check if we have valid content to composite
    bool hasValidOverlay() const { return has_content_; }

private:
    bool createTexture();
    bool createShader();
    void destroyTexture();

    GLContext* ctx_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Overlay texture
    GLuint texture_ = 0;
    bool has_content_ = false;

    // Double-buffered PBOs for async texture upload (software path)
    GLuint pbos_[2] = {0, 0};
    int current_pbo_ = 0;
    void* pbo_mapped_ = nullptr;
    bool staging_pending_ = false;

    // GPU texture sharing
    bool use_gpu_path_ = false;

#ifdef __APPLE__
    // IOSurface support (macOS)
    IOSurfaceRef pending_iosurface_ = nullptr;
    int pending_iosurface_width_ = 0;
    int pending_iosurface_height_ = 0;
    bool iosurface_queued_ = false;
    GLuint texture_rect_ = 0;  // GL_TEXTURE_RECTANGLE for IOSurface
#else
    // DMA-BUF support (Linux, triple buffered)
    bool dmabuf_supported_ = true;
    std::vector<AcceleratedPaintInfo> pending_dmabufs_;
    static constexpr int NUM_BUFFERS = 3;
    EGLImage images_[NUM_BUFFERS] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    int dmabuf_fds_[NUM_BUFFERS] = {-1, -1, -1};
    int buffer_index_ = 0;
    std::chrono::steady_clock::time_point last_resize_time_;
#endif

    // Thread safety
    std::mutex mutex_;

    // Shader program
    GLuint program_ = 0;
    GLint alpha_loc_ = -1;

#ifdef __APPLE__
    // Rectangle texture shader (IOSurface path)
    GLuint program_rect_ = 0;
    GLint alpha_loc_rect_ = -1;
    GLint texsize_loc_rect_ = -1;
#endif

    // VAO for fullscreen quad
    GLuint vao_ = 0;
};
