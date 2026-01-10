#pragma once

#include "vulkan_context.h"
#ifdef __APPLE__
#include "macos_layer.h"
using VideoSurface = MacOSVideoLayer;
#else
#include "wayland_subsurface.h"
using VideoSurface = WaylandSubsurface;
#endif
#include <string>
#include <functional>
#include <atomic>

struct mpv_handle;
struct mpv_render_context;

class MpvPlayerVk {
public:
    using RedrawCallback = std::function<void()>;

    MpvPlayerVk();
    ~MpvPlayerVk();

    bool init(VulkanContext* vk, VideoSurface* subsurface = nullptr);
    void cleanup();
    bool loadFile(const std::string& path, double startSeconds = 0.0);

    // Check if mpv has a new frame ready to render
    bool hasFrame() const;

    // Render to swapchain image
    void render(VkImage image, VkImageView view, uint32_t width, uint32_t height, VkFormat format);

    // Playback control
    void stop();
    void pause();
    void play();
    void seek(double seconds);
    void setVolume(int volume);
    void setMuted(bool muted);

    // State queries
    double getPosition() const;
    double getDuration() const;
    bool isPaused() const;
    bool isPlaying() const { return playing_; }

    void setRedrawCallback(RedrawCallback cb) { redraw_callback_ = cb; }
    bool needsRedraw() const { return needs_redraw_.load(); }
    void clearRedrawFlag() { needs_redraw_ = false; }

    bool isHdr() const { return subsurface_ && subsurface_->isHdr(); }
    VideoSurface* subsurface() const { return subsurface_; }

private:
    static void onMpvRedraw(void* ctx);

    VulkanContext* vk_ = nullptr;
    VideoSurface* subsurface_ = nullptr;
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ctx_ = nullptr;

    RedrawCallback redraw_callback_;
    std::atomic<bool> needs_redraw_{false};
    bool playing_ = false;
};
