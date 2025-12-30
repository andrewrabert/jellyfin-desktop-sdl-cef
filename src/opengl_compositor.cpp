#include "opengl_compositor.h"
#include <iostream>
#include <cstring>
#include <unistd.h>

static auto _log_start = std::chrono::steady_clock::now();
#define COMP_LOG(msg) do { \
    auto _now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _log_start).count(); \
    std::cerr << "[" << _now << "ms] [GLCompositor] " << msg << std::endl; std::cerr.flush(); \
} while(0)

// Vertex shader - fullscreen triangle
static const char* vert_src = R"(#version 300 es
out vec2 texCoord;
void main() {
    // Generate fullscreen triangle from vertex ID
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    texCoord = vec2(pos.x, 1.0 - pos.y);  // Flip Y for correct orientation
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Fragment shader - sample texture with alpha
static const char* frag_src = R"(#version 300 es
precision mediump float;
in vec2 texCoord;
out vec4 fragColor;
uniform sampler2D overlayTex;
uniform float alpha;
void main() {
    vec4 color = texture(overlayTex, texCoord);
    fragColor = color * alpha;  // Premultiplied alpha
}
)";

OpenGLCompositor::OpenGLCompositor() = default;

OpenGLCompositor::~OpenGLCompositor() {
    cleanup();
}

bool OpenGLCompositor::init(EGLContext_* egl, uint32_t width, uint32_t height) {
    egl_ = egl;
    width_ = width;
    height_ = height;

    if (!createTexture()) return false;
    if (!createShader()) return false;

    // Create VAO (required for GLES 3.0)
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

    // Allocate texture storage (BGRA format)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);

    // Allocate staging buffer
    staging_buffer_.resize(width_ * height_ * 4);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "[GLCompositor] Failed to create texture: " << err << std::endl;
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

    return true;
}

void OpenGLCompositor::updateOverlay(const void* data, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (width != static_cast<int>(width_) || height != static_cast<int>(height_)) {
        return;
    }

    std::memcpy(staging_buffer_.data(), data, width * height * 4);
    staging_pending_ = true;
}

void* OpenGLCompositor::getStagingBuffer(int width, int height) {
    if (width != static_cast<int>(width_) || height != static_cast<int>(height_)) {
        return nullptr;
    }
    return staging_buffer_.data();
}

bool OpenGLCompositor::flushOverlay() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!staging_pending_ || !texture_) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_BGRA_EXT, GL_UNSIGNED_BYTE, staging_buffer_.data());

    staging_pending_ = false;
    has_content_ = true;
    return true;
}

bool OpenGLCompositor::updateOverlayFromDmaBuf(const AcceleratedPaintInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (info.planes.empty() || info.planes[0].fd < 0) {
        return false;
    }

    // Skip DMA-BUF imports briefly after resize
    auto since_resize = std::chrono::steady_clock::now() - last_resize_time_;
    if (since_resize < std::chrono::milliseconds(150)) {
        COMP_LOG("updateOverlayFromDmaBuf: skipping (resize cooldown)");
        close(info.planes[0].fd);
        return false;
    }

    // Skip if size doesn't match
    if (static_cast<uint32_t>(info.width) != width_ || static_cast<uint32_t>(info.height) != height_) {
        COMP_LOG("updateOverlayFromDmaBuf: " << info.width << "x" << info.height << " (want: " << width_ << "x" << height_ << ") - skipping");
        close(info.planes[0].fd);
        return false;
    }

    if (!dmabuf_supported_ || !egl_->hasDmaBufImport()) {
        close(info.planes[0].fd);
        return false;
    }

    COMP_LOG("updateOverlayFromDmaBuf: " << info.width << "x" << info.height);

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

    EGLImage image = egl_->eglCreateImageKHR(egl_->display(), EGL_NO_CONTEXT,
                                               EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);

    if (image == EGL_NO_IMAGE_KHR) {
        COMP_LOG("updateOverlayFromDmaBuf: eglCreateImageKHR failed");
        dmabuf_supported_ = false;
        close(info.planes[0].fd);
        return false;
    }

    // Bind image to texture
    glBindTexture(GL_TEXTURE_2D, texture_);
    egl_->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        COMP_LOG("updateOverlayFromDmaBuf: glEGLImageTargetTexture2DOES failed: " << err);
        egl_->eglDestroyImageKHR(egl_->display(), image);
        dmabuf_supported_ = false;
        close(info.planes[0].fd);
        return false;
    }

    // EGL owns the fd now after successful import
    egl_->eglDestroyImageKHR(egl_->display(), image);
    close(info.planes[0].fd);

    COMP_LOG("updateOverlayFromDmaBuf: done");
    has_content_ = true;
    return true;
}

void OpenGLCompositor::composite(uint32_t width, uint32_t height, float alpha) {
    if (!has_content_ || !program_ || !texture_) {
        return;
    }

    glViewport(0, 0, width, height);

    // Enable blending with premultiplied alpha
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(program_);
    glUniform1f(alpha_loc_, alpha);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);

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
    last_resize_time_ = std::chrono::steady_clock::now();

    destroyTexture();

    width_ = width;
    height_ = height;
    has_content_ = false;

    createTexture();
    COMP_LOG("resize: done");
}

void OpenGLCompositor::destroyTexture() {
    if (texture_) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    staging_buffer_.clear();
}

void OpenGLCompositor::cleanup() {
    if (!egl_) return;

    destroyTexture();

    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }

    egl_ = nullptr;
}
