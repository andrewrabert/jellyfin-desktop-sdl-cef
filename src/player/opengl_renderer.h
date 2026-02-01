#pragma once
#include "video_renderer.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <atomic>
#include <mutex>

class MpvPlayerGL;
class EGLContext_;

class OpenGLRenderer : public VideoRenderer {
public:
    explicit OpenGLRenderer(MpvPlayerGL* player);
    ~OpenGLRenderer();

    // Initialize for threaded rendering (creates shared context + FBO)
    bool initThreaded(EGLContext_* egl);

    bool hasFrame() const override;

    // Render video to FBO (called from video thread)
    bool render(int width, int height) override;

    // Composite video texture to screen (called from main thread)
    void composite(int width, int height);

    void setVisible(bool) override {}
    void resize(int width, int height) override;
    void setDestinationSize(int, int) override {}
    void setColorspace() override {}
    void cleanup() override;
    float getClearAlpha(bool video_ready) const override;
    bool isHdr() const override { return false; }

    bool isThreaded() const { return threaded_; }

private:
    void createFBO(int width, int height);
    void destroyFBO();

    MpvPlayerGL* player_;
    EGLContext_* egl_ = nullptr;
    EGLContext shared_ctx_ = EGL_NO_CONTEXT;
    bool threaded_ = false;

    // FBO for offscreen rendering
    GLuint fbo_ = 0;
    GLuint texture_ = 0;
    GLuint depth_rb_ = 0;
    int fbo_width_ = 0;
    int fbo_height_ = 0;

    // Shader for compositing
    GLuint composite_program_ = 0;
    GLuint composite_vao_ = 0;

    std::mutex fbo_mutex_;
    std::atomic<bool> has_rendered_{false};
    std::atomic<GLuint> current_texture_{0};  // Atomic for lock-free composite access
};
