#pragma once

#include <GL/glew.h>
#include <string>
#include <functional>

struct mpv_handle;
struct mpv_render_context;

class MpvPlayer {
public:
    using RedrawCallback = std::function<void()>;

    MpvPlayer();
    ~MpvPlayer();

    bool init(int width, int height);
    bool loadFile(const std::string& path);
    void render();

    void setRedrawCallback(RedrawCallback cb) { redraw_callback_ = cb; }
    GLuint getTexture() const { return texture_; }
    bool needsRedraw() const { return needs_redraw_; }
    void clearRedrawFlag() { needs_redraw_ = false; }

private:
    static void onMpvRedraw(void* ctx);
    static void* getProcAddress(void* ctx, const char* name);

    mpv_handle* mpv_ = nullptr;
    mpv_render_context* mpv_gl_ = nullptr;

    GLuint fbo_ = 0;
    GLuint texture_ = 0;
    int width_ = 0;
    int height_ = 0;

    RedrawCallback redraw_callback_;
    bool needs_redraw_ = false;
};
