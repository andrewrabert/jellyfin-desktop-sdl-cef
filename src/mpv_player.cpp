#include "mpv_player.h"
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <SDL2/SDL.h>
#include <iostream>
#include <clocale>

MpvPlayer::MpvPlayer() = default;

MpvPlayer::~MpvPlayer() {
    if (mpv_gl_) {
        mpv_render_context_free(mpv_gl_);
    }
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
    }
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
    }
    if (texture_) {
        glDeleteTextures(1, &texture_);
    }
}

void* MpvPlayer::getProcAddress(void* ctx, const char* name) {
    (void)ctx;
    return (void*)SDL_GL_GetProcAddress(name);
}

void MpvPlayer::onMpvRedraw(void* ctx) {
    MpvPlayer* player = static_cast<MpvPlayer*>(ctx);
    player->needs_redraw_ = true;
    if (player->redraw_callback_) {
        player->redraw_callback_();
    }
}

bool MpvPlayer::init(int width, int height) {
    width_ = width;
    height_ = height;

    // libmpv requires C locale for numeric formatting
    std::setlocale(LC_NUMERIC, "C");

    // Create mpv instance
    mpv_ = mpv_create();
    if (!mpv_) {
        std::cerr << "mpv_create failed" << std::endl;
        return false;
    }

    // Set options before initialization
    mpv_set_option_string(mpv_, "vo", "libmpv");
    mpv_set_option_string(mpv_, "hwdec", "auto");
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "terminal", "yes");
    mpv_set_option_string(mpv_, "msg-level", "all=v");  // Verbose logging

    if (mpv_initialize(mpv_) < 0) {
        std::cerr << "mpv_initialize failed" << std::endl;
        return false;
    }

    // Create OpenGL render context
    mpv_opengl_init_params gl_init_params{
        getProcAddress,
        nullptr,
    };

    int advanced_control = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if (mpv_render_context_create(&mpv_gl_, mpv_, params) < 0) {
        std::cerr << "mpv_render_context_create failed" << std::endl;
        return false;
    }

    mpv_render_context_set_update_callback(mpv_gl_, onMpvRedraw, this);

    // Create FBO and texture for mpv to render into
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "FBO incomplete" << std::endl;
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return true;
}

bool MpvPlayer::loadFile(const std::string& path) {
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    int ret = mpv_command(mpv_, cmd);
    if (ret >= 0) {
        playing_ = true;
    }
    return ret >= 0;
}

void MpvPlayer::stop() {
    if (!mpv_) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command(mpv_, cmd);
    playing_ = false;
}

void MpvPlayer::pause() {
    if (!mpv_) return;
    int pause = 1;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
}

void MpvPlayer::play() {
    if (!mpv_) return;
    int pause = 0;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
}

void MpvPlayer::seek(double seconds) {
    if (!mpv_) return;
    std::string time_str = std::to_string(seconds);
    const char* cmd[] = {"seek", time_str.c_str(), "absolute", nullptr};
    mpv_command(mpv_, cmd);
}

void MpvPlayer::setVolume(int volume) {
    if (!mpv_) return;
    double vol = static_cast<double>(volume);
    mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void MpvPlayer::setMuted(bool muted) {
    if (!mpv_) return;
    int m = muted ? 1 : 0;
    mpv_set_property(mpv_, "mute", MPV_FORMAT_FLAG, &m);
}

double MpvPlayer::getPosition() const {
    if (!mpv_) return 0;
    double pos = 0;
    mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

double MpvPlayer::getDuration() const {
    if (!mpv_) return 0;
    double dur = 0;
    mpv_get_property(mpv_, "duration", MPV_FORMAT_DOUBLE, &dur);
    return dur;
}

bool MpvPlayer::isPaused() const {
    if (!mpv_) return false;
    int paused = 0;
    mpv_get_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused);
    return paused != 0;
}

void MpvPlayer::render() {
    if (!mpv_gl_) return;

    // Check if mpv wants to render
    uint64_t flags = mpv_render_context_update(mpv_gl_);
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) {
        return;
    }

    // Render to our FBO
    mpv_opengl_fbo fbo_params{
        static_cast<int>(fbo_),
        width_,
        height_,
        0  // internal_format (0 = default)
    };

    int flip_y = 0;
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo_params},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    mpv_render_context_render(mpv_gl_, render_params);
}
