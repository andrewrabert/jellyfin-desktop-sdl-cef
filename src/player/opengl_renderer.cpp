#include "opengl_renderer.h"
#include "mpv/mpv_player_gl.h"
#include "context/egl_context.h"
#include "logging.h"

// Shader for compositing video texture (fullscreen triangle)
static const char* composite_vert = R"(#version 300 es
out vec2 vTexCoord;
void main() {
    // Fullscreen triangle: vertices at (-1,-1), (3,-1), (-1,3)
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vTexCoord = pos;
    vTexCoord.y = 1.0 - vTexCoord.y;  // Flip Y for video
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* composite_frag = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D videoTex;
void main() {
    fragColor = texture(videoTex, vTexCoord);
}
)";

OpenGLRenderer::OpenGLRenderer(MpvPlayerGL* player) : player_(player) {}

OpenGLRenderer::~OpenGLRenderer() {
    cleanup();
}

bool OpenGLRenderer::initThreaded(EGLContext_* egl) {
    egl_ = egl;
    shared_ctx_ = egl->createSharedContext();
    if (shared_ctx_ == EGL_NO_CONTEXT) {
        LOG_ERROR(LOG_VIDEO, "Failed to create shared EGL context for video");
        return false;
    }
    threaded_ = true;
    LOG_INFO(LOG_VIDEO, "OpenGLRenderer initialized for threaded rendering");
    return true;
}

bool OpenGLRenderer::hasFrame() const {
    return player_->hasFrame();
}

void OpenGLRenderer::createFBO(int width, int height) {
    if (fbo_ && fbo_width_ == width && fbo_height_ == height) {
        return;  // Already have correct size
    }

    destroyFBO();

    glGenFramebuffers(1, &fbo_);
    glGenTextures(1, &texture_);
    glGenRenderbuffers(1, &depth_rb_);

    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb_);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR(LOG_VIDEO, "FBO incomplete: 0x%x", status);
        destroyFBO();
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fbo_width_ = width;
    fbo_height_ = height;
    current_texture_.store(texture_);  // Publish texture atomically
    LOG_INFO(LOG_VIDEO, "Created video FBO: %dx%d", width, height);
}

void OpenGLRenderer::destroyFBO() {
    current_texture_.store(0);  // Clear atomic first so composite() won't use stale texture
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (texture_) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    if (depth_rb_) {
        glDeleteRenderbuffers(1, &depth_rb_);
        depth_rb_ = 0;
    }
    fbo_width_ = 0;
    fbo_height_ = 0;
}

bool OpenGLRenderer::render(int width, int height) {
    if (threaded_) {
        // Make shared context current on this thread
        if (!egl_->makeCurrent(shared_ctx_)) {
            LOG_ERROR(LOG_VIDEO, "Failed to make shared context current");
            return false;
        }

        // Create/resize FBO if needed (brief lock)
        {
            std::lock_guard<std::mutex> lock(fbo_mutex_);
            createFBO(width, height);
            if (!fbo_) {
                LOG_ERROR(LOG_VIDEO, "FBO creation failed");
                egl_->makeCurrent(EGL_NO_CONTEXT);
                return false;
            }
        }

        // Render without holding lock - FBO/texture IDs don't change during render
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, width, height);
        player_->render(width, height, fbo_, false);  // No flip - FBO is top-down

        // Flush commands (non-blocking, just ensures they're submitted)
        glFlush();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        egl_->makeCurrent(EGL_NO_CONTEXT);
        has_rendered_.store(true);
    } else {
        // Direct rendering to default framebuffer
        player_->render(width, height, 0, true);  // Flip for screen
        has_rendered_.store(true);
    }
    return true;
}

void OpenGLRenderer::composite(int width, int height) {
    if (!threaded_ || !has_rendered_.load() || !texture_) {
        return;
    }

    // Create composite shader if needed
    if (!composite_program_) {
        GLuint vert = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vert, 1, &composite_vert, nullptr);
        glCompileShader(vert);

        GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(frag, 1, &composite_frag, nullptr);
        glCompileShader(frag);

        composite_program_ = glCreateProgram();
        glAttachShader(composite_program_, vert);
        glAttachShader(composite_program_, frag);
        glLinkProgram(composite_program_);

        glDeleteShader(vert);
        glDeleteShader(frag);

        glGenVertexArrays(1, &composite_vao_);
    }

    // Use atomically published texture ID
    GLuint tex = current_texture_.load();
    if (!tex) return;

    glUseProgram(composite_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(composite_program_, "videoTex"), 0);

    glBindVertexArray(composite_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
}

void OpenGLRenderer::resize(int width, int height) {
    // FBO will be recreated on next render if size changed
    (void)width;
    (void)height;
}

void OpenGLRenderer::cleanup() {
    if (threaded_ && egl_) {
        egl_->makeCurrent(shared_ctx_);
    }

    destroyFBO();

    if (composite_program_) {
        glDeleteProgram(composite_program_);
        composite_program_ = 0;
    }
    if (composite_vao_) {
        glDeleteVertexArrays(1, &composite_vao_);
        composite_vao_ = 0;
    }

    if (threaded_ && egl_) {
        egl_->makeCurrent(EGL_NO_CONTEXT);
        egl_->destroyContext(shared_ctx_);
        shared_ctx_ = EGL_NO_CONTEXT;
    }

    threaded_ = false;
    has_rendered_.store(false);
}

float OpenGLRenderer::getClearAlpha(bool video_ready) const {
    // For threaded mode, use transparent clear so video shows through
    // For sync mode, video renders first so use opaque clear
    if (threaded_) {
        return video_ready ? 0.0f : 1.0f;
    }
    return 1.0f;
}
