#include "compositor/opengl_compositor.h"
#include <iostream>
#include <cstring>

#ifdef __APPLE__
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
// macOS gl3.h defines GL_BGRA, not GL_BGRA_EXT
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT GL_BGRA
#endif
#else
#include <unistd.h>
#endif

static auto _log_start = std::chrono::steady_clock::now();
#define COMP_LOG(msg) do { \
    auto _now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _log_start).count(); \
    std::cerr << "[" << _now << "ms] [GLCompositor] " << msg << std::endl; std::cerr.flush(); \
} while(0)

#ifdef __APPLE__
// macOS: Desktop OpenGL 3.2 Core with GL_TEXTURE_2D (software path)
static const char* vert_src = R"(#version 150
out vec2 texCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_src = R"(#version 150
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    fragColor = color * alpha;
}
)";

// macOS: Desktop OpenGL 3.2 Core with GL_TEXTURE_RECTANGLE (IOSurface path)
static const char* vert_rect_src = R"(#version 150
out vec2 texCoord;
uniform vec2 texSize;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, 1.0 - pos.y) * texSize;  // Non-normalized coords
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_rect_src = R"(#version 150
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2DRect overlayTex;
uniform float alpha;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    fragColor = color * alpha;
}
)";
#else
// Linux: OpenGL ES 3.0
static const char* vert_src = R"(#version 300 es
out vec2 texCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* frag_src = R"(#version 300 es
precision mediump float;
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    // CEF provides BGRA, uploaded as RGBA - swizzle back
    fragColor = color.bgra * alpha;
}
)";
#endif

OpenGLCompositor::OpenGLCompositor() = default;

OpenGLCompositor::~OpenGLCompositor() {
    cleanup();
}

bool OpenGLCompositor::init(GLContext* ctx, uint32_t width, uint32_t height, bool use_gpu_path) {
    ctx_ = ctx;
    width_ = width;
    height_ = height;
    use_gpu_path_ = use_gpu_path;

    if (!createTexture()) return false;
    if (!createShader()) return false;

    // Create VAO (required for GLES 3.0 / OpenGL core)
    glGenVertexArrays(1, &vao_);

    return true;
}

bool OpenGLCompositor::createTexture() {
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifdef __APPLE__
    // IOSurface path: also create rectangle texture for IOSurface binding
    if (use_gpu_path_) {
        glGenTextures(1, &texture_rect_);
        glBindTexture(GL_TEXTURE_RECTANGLE, texture_rect_);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return true;
    }

    // Software path: allocate texture storage and PBOs
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
#else
    // DMA-BUF path: EGLImage provides texture backing, skip PBO setup
    if (use_gpu_path_) {
        return true;
    }

    // Software path: allocate texture storage and PBOs
    // Use GL_RGBA (universally supported) - shader swizzles BGRA->RGBA
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLenum texErr = glGetError();
    if (texErr != GL_NO_ERROR) {
        std::cerr << "[GLCompositor] glTexImage2D failed: " << texErr << std::endl;
        return false;
    }
#endif

    // Create double-buffered PBOs for async upload
    size_t pbo_size = width_ * height_ * 4;
    glGenBuffers(2, pbos_);
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[i]);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, pbo_size, nullptr, GL_STREAM_DRAW);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    GLenum pboErr = glGetError();
    if (pboErr != GL_NO_ERROR) {
        std::cerr << "[GLCompositor] PBO creation failed: " << pboErr << std::endl;
        return false;
    }

    // Map the first PBO for writing
    current_pbo_ = 0;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[current_pbo_]);
    pbo_mapped_ = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, pbo_size,
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "[GLCompositor] glMapBufferRange failed: " << err << std::endl;
        return false;
    }

    return true;
}

bool OpenGLCompositor::createShader() {
    // Compile vertex shader
    GLuint vert = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vert, 1, &vert_src, nullptr);
    glCompileShader(vert);

    GLint status;
    glGetShaderiv(vert, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(vert, 512, nullptr, log);
        std::cerr << "[GLCompositor] Vertex shader error: " << log << std::endl;
        glDeleteShader(vert);
        return false;
    }

    // Compile fragment shader
    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(frag, 1, &frag_src, nullptr);
    glCompileShader(frag);

    glGetShaderiv(frag, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(frag, 512, nullptr, log);
        std::cerr << "[GLCompositor] Fragment shader error: " << log << std::endl;
        glDeleteShader(vert);
        glDeleteShader(frag);
        return false;
    }

    // Link program
    program_ = glCreateProgram();
    glAttachShader(program_, vert);
    glAttachShader(program_, frag);
    glLinkProgram(program_);

    glGetProgramiv(program_, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(program_, 512, nullptr, log);
        std::cerr << "[GLCompositor] Program link error: " << log << std::endl;
        glDeleteShader(vert);
        glDeleteShader(frag);
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    // Get uniform locations
    alpha_loc_ = glGetUniformLocation(program_, "alpha");

#ifdef __APPLE__
    // Create rectangle texture shader for IOSurface path
    if (use_gpu_path_) {
        GLuint vert_rect = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vert_rect, 1, &vert_rect_src, nullptr);
        glCompileShader(vert_rect);

        glGetShaderiv(vert_rect, GL_COMPILE_STATUS, &status);
        if (!status) {
            char log[512];
            glGetShaderInfoLog(vert_rect, 512, nullptr, log);
            std::cerr << "[GLCompositor] Rect vertex shader error: " << log << std::endl;
            glDeleteShader(vert_rect);
            return false;
        }

        GLuint frag_rect = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(frag_rect, 1, &frag_rect_src, nullptr);
        glCompileShader(frag_rect);

        glGetShaderiv(frag_rect, GL_COMPILE_STATUS, &status);
        if (!status) {
            char log[512];
            glGetShaderInfoLog(frag_rect, 512, nullptr, log);
            std::cerr << "[GLCompositor] Rect fragment shader error: " << log << std::endl;
            glDeleteShader(vert_rect);
            glDeleteShader(frag_rect);
            return false;
        }

        program_rect_ = glCreateProgram();
        glAttachShader(program_rect_, vert_rect);
        glAttachShader(program_rect_, frag_rect);
        glLinkProgram(program_rect_);

        glGetProgramiv(program_rect_, GL_LINK_STATUS, &status);
        if (!status) {
            char log[512];
            glGetProgramInfoLog(program_rect_, 512, nullptr, log);
            std::cerr << "[GLCompositor] Rect program link error: " << log << std::endl;
            glDeleteShader(vert_rect);
            glDeleteShader(frag_rect);
            glDeleteProgram(program_rect_);
            program_rect_ = 0;
            return false;
        }

        glDeleteShader(vert_rect);
        glDeleteShader(frag_rect);

        alpha_loc_rect_ = glGetUniformLocation(program_rect_, "alpha");
        texsize_loc_rect_ = glGetUniformLocation(program_rect_, "texSize");
    }
#endif

    return true;
}

void OpenGLCompositor::updateOverlay(const void* data, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (width != static_cast<int>(width_) || height != static_cast<int>(height_)) {
        return;
    }

    if (pbo_mapped_) {
        std::memcpy(pbo_mapped_, data, width * height * 4);
        staging_pending_ = true;
    }
}

void* OpenGLCompositor::getStagingBuffer(int width, int height) {
    if (width != static_cast<int>(width_) || height != static_cast<int>(height_)) {
        return nullptr;
    }
    return pbo_mapped_;
}

bool OpenGLCompositor::flushOverlay() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!staging_pending_ || !texture_) {
        return false;
    }

    size_t pbo_size = width_ * height_ * 4;

    // Unmap current PBO and start async DMA transfer to texture
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[current_pbo_]);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

    glBindTexture(GL_TEXTURE_2D, texture_);
    // With PBO bound, last arg is offset into PBO, not pointer
    // Upload as RGBA - shader swizzles BGRA->RGBA
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Swap to next PBO and map it for next frame's writes
    current_pbo_ = 1 - current_pbo_;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[current_pbo_]);
    pbo_mapped_ = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, pbo_size,
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    staging_pending_ = false;
    has_content_ = true;
    return true;
}

#ifdef __APPLE__
// macOS: IOSurface import
void OpenGLCompositor::queueIOSurface(IOSurfaceRef surface, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Release previous queued surface if not yet imported
    if (iosurface_queued_ && pending_iosurface_) {
        CFRelease(pending_iosurface_);
    }

    pending_iosurface_ = surface;
    if (surface) CFRetain(surface);
    pending_iosurface_width_ = width;
    pending_iosurface_height_ = height;
    iosurface_queued_ = true;
}

bool OpenGLCompositor::importQueuedIOSurface() {
    IOSurfaceRef surface;
    int surf_width, surf_height;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!iosurface_queued_) return false;
        surface = pending_iosurface_;
        surf_width = pending_iosurface_width_;
        surf_height = pending_iosurface_height_;
        pending_iosurface_ = nullptr;
        iosurface_queued_ = false;
    }

    if (!surface) {
        return false;
    }

    // Skip if size doesn't match
    if (static_cast<uint32_t>(surf_width) != width_ || static_cast<uint32_t>(surf_height) != height_) {
        COMP_LOG("importQueuedIOSurface: " << surf_width << "x" << surf_height << " (want: " << width_ << "x" << height_ << ") - skipping");
        CFRelease(surface);
        return false;
    }

    COMP_LOG("importQueuedIOSurface: " << surf_width << "x" << surf_height);

    // Bind IOSurface to rectangle texture
    glBindTexture(GL_TEXTURE_RECTANGLE, texture_rect_);

    CGLContextObj cgl_ctx = ctx_->cglContext();
    CGLError cgl_err = CGLTexImageIOSurface2D(cgl_ctx, GL_TEXTURE_RECTANGLE,
                                               GL_RGBA, surf_width, surf_height,
                                               GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                                               surface, 0);

    CFRelease(surface);

    if (cgl_err != kCGLNoError) {
        COMP_LOG("importQueuedIOSurface: CGLTexImageIOSurface2D failed: " << cgl_err);
        return false;
    }

    COMP_LOG("importQueuedIOSurface: done");
    has_content_ = true;
    return true;
}

#else
// Linux: DMA-BUF import
void OpenGLCompositor::queueDmaBuf(const AcceleratedPaintInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    COMP_LOG("queueDmaBuf: fd=" << info.planes[0].fd << " size=" << info.width << "x" << info.height << " pending=" << pending_dmabufs_.size());
    pending_dmabufs_.push_back(info);
}

bool OpenGLCompositor::importQueuedDmaBuf() {
    AcceleratedPaintInfo info;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (pending_dmabufs_.empty()) return false;

        // Wait 200ms after resize before importing (CEF needs time to stabilize)
        auto now = std::chrono::steady_clock::now();
        auto since_resize = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_resize_time_).count();
        if (since_resize < 200) {
            // Clear all pending - they're from the unstable period
            for (auto& dmabuf : pending_dmabufs_) {
                for (auto& plane : dmabuf.planes) {
                    if (plane.fd >= 0) close(plane.fd);
                }
            }
            pending_dmabufs_.clear();
            return false;
        }

        // Find first entry matching current size
        auto it = pending_dmabufs_.begin();
        for (; it != pending_dmabufs_.end(); ++it) {
            if (static_cast<uint32_t>(it->width) == width_ &&
                static_cast<uint32_t>(it->height) == height_) break;
        }

        if (it == pending_dmabufs_.end()) {
            // No match yet - keep waiting
            return false;
        }

        // Close everything before the match (stale sizes)
        for (auto i = pending_dmabufs_.begin(); i != it; ++i) {
            COMP_LOG("importQueuedDmaBuf: closing stale fd=" << i->planes[0].fd << " size=" << i->width << "x" << i->height);
            for (auto& plane : i->planes) {
                if (plane.fd >= 0) close(plane.fd);
            }
        }

        // Extract the matching entry
        info = std::move(*it);
        pending_dmabufs_.erase(pending_dmabufs_.begin(), it + 1);
    }

    if (info.planes.empty() || info.planes[0].fd < 0) {
        return false;
    }

    if (!dmabuf_supported_ || !ctx_->hasDmaBufImport()) {
        close(info.planes[0].fd);
        return false;
    }

    COMP_LOG("importQueuedDmaBuf: fd=" << info.planes[0].fd << " size=" << info.width << "x" << info.height << " stride=" << info.planes[0].stride << " offset=" << info.planes[0].offset << " modifier=0x" << std::hex << info.modifier << std::dec);

    // Convert CEF format to DRM fourcc
    uint32_t drm_format = DRM_FORMAT_ARGB8888;  // Default, CEF typically uses BGRA

    EGLint attribs[] = {
        EGL_WIDTH, static_cast<EGLint>(info.width),
        EGL_HEIGHT, static_cast<EGLint>(info.height),
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(drm_format),
        EGL_DMA_BUF_PLANE0_FD_EXT, info.planes[0].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(info.planes[0].offset),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(info.planes[0].stride),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(info.modifier & 0xFFFFFFFF),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>(info.modifier >> 32),
        EGL_NONE
    };

    EGLImage image = ctx_->eglCreateImageKHR(ctx_->display(), EGL_NO_CONTEXT,
                                               EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);

    if (image == EGL_NO_IMAGE_KHR) {
        EGLint err = eglGetError();
        COMP_LOG("importQueuedDmaBuf: eglCreateImageKHR failed, EGL error: 0x" << std::hex << err << std::dec);
        // Don't disable dmabuf_supported_ - might be transient
        close(info.planes[0].fd);
        return false;
    }

    // Bind image to texture
    glBindTexture(GL_TEXTURE_2D, texture_);
    ctx_->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        COMP_LOG("importQueuedDmaBuf: glEGLImageTargetTexture2DOES failed: 0x" << std::hex << err << std::dec);
        ctx_->eglDestroyImageKHR(ctx_->display(), image);
        close(info.planes[0].fd);
        return false;
    }

    // Release oldest buffer in ring, store new one (triple buffering)
    if (images_[buffer_index_] != EGL_NO_IMAGE_KHR) {
        ctx_->eglDestroyImageKHR(ctx_->display(), images_[buffer_index_]);
    }
    if (dmabuf_fds_[buffer_index_] >= 0) {
        close(dmabuf_fds_[buffer_index_]);
    }
    images_[buffer_index_] = image;
    dmabuf_fds_[buffer_index_] = info.planes[0].fd;
    buffer_index_ = (buffer_index_ + 1) % NUM_BUFFERS;

    COMP_LOG("importQueuedDmaBuf: done");
    has_content_ = true;
    return true;
}
#endif

void OpenGLCompositor::composite(uint32_t width, uint32_t height, float alpha) {
    if (!has_content_ || !program_) {
        return;
    }

    glViewport(0, 0, width, height);

    // Enable blending with premultiplied alpha
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

#ifdef __APPLE__
    // macOS: Use rectangle texture shader for IOSurface path
    if (use_gpu_path_ && program_rect_ && texture_rect_) {
        glUseProgram(program_rect_);
        glUniform1f(alpha_loc_rect_, alpha);
        glUniform2f(texsize_loc_rect_, static_cast<float>(pending_iosurface_width_),
                    static_cast<float>(pending_iosurface_height_));

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_RECTANGLE, texture_rect_);
    } else {
        glUseProgram(program_);
        glUniform1f(alpha_loc_, alpha);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_);
    }
#else
    glUseProgram(program_);
    glUniform1f(alpha_loc_, alpha);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
#endif

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
}

void OpenGLCompositor::resize(uint32_t width, uint32_t height) {
    COMP_LOG("resize: " << width << "x" << height << " (current: " << width_ << "x" << height_ << ")");

    if (width == width_ && height == height_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

#ifdef __APPLE__
    // Clear any pending IOSurface (stale after resize)
    if (iosurface_queued_ && pending_iosurface_) {
        CFRelease(pending_iosurface_);
        pending_iosurface_ = nullptr;
        iosurface_queued_ = false;
    }
#else
    // Don't close pending_dmabufs_ here - they'll be cleaned up when a
    // matching frame arrives in importQueuedDmaBuf()

    // Wait for GPU to finish using current buffers before releasing
    glFinish();

    // Release all EGLImages in ring buffer
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (images_[i] != EGL_NO_IMAGE_KHR) {
            ctx_->eglDestroyImageKHR(ctx_->display(), images_[i]);
            images_[i] = EGL_NO_IMAGE_KHR;
        }
        if (dmabuf_fds_[i] >= 0) {
            close(dmabuf_fds_[i]);
            dmabuf_fds_[i] = -1;
        }
    }
    buffer_index_ = 0;
    last_resize_time_ = std::chrono::steady_clock::now();
#endif

    destroyTexture();

    width_ = width;
    height_ = height;
    has_content_ = false;

    createTexture();
    COMP_LOG("resize: done");
}

void OpenGLCompositor::destroyTexture() {
    // Unmap and delete PBOs
    if (pbo_mapped_) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[current_pbo_]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        pbo_mapped_ = nullptr;
    }
    if (pbos_[0]) {
        glDeleteBuffers(2, pbos_);
        pbos_[0] = pbos_[1] = 0;
    }
    current_pbo_ = 0;

    if (texture_) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }

#ifdef __APPLE__
    if (texture_rect_) {
        glDeleteTextures(1, &texture_rect_);
        texture_rect_ = 0;
    }
#endif
}

void OpenGLCompositor::cleanup() {
    if (!ctx_) return;

#ifdef __APPLE__
    // Release any pending IOSurface
    if (pending_iosurface_) {
        CFRelease(pending_iosurface_);
        pending_iosurface_ = nullptr;
    }
#else
    // Release all EGLImages in ring buffer
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (images_[i] != EGL_NO_IMAGE_KHR) {
            ctx_->eglDestroyImageKHR(ctx_->display(), images_[i]);
            images_[i] = EGL_NO_IMAGE_KHR;
        }
        if (dmabuf_fds_[i] >= 0) {
            close(dmabuf_fds_[i]);
            dmabuf_fds_[i] = -1;
        }
    }

    // Drain pending DMA-BUF list
    for (auto& info : pending_dmabufs_) {
        COMP_LOG("cleanup: closing fd=" << info.planes[0].fd << " size=" << info.width << "x" << info.height);
        for (auto& plane : info.planes) {
            if (plane.fd >= 0) close(plane.fd);
        }
    }
    pending_dmabufs_.clear();
#endif

    destroyTexture();

    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
#ifdef __APPLE__
    if (program_rect_) {
        glDeleteProgram(program_rect_);
        program_rect_ = 0;
    }
#endif
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }

    ctx_ = nullptr;
}
