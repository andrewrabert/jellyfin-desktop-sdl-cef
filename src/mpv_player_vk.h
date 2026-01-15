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
    // Event callbacks (like Qt signals)
    using PositionCallback = std::function<void(double ms)>;
    using DurationCallback = std::function<void(double ms)>;
    using StateCallback = std::function<void(bool paused)>;
    using PlaybackCallback = std::function<void()>;  // playing, finished, error
    using SeekCallback = std::function<void(double ms)>;  // seek completed with position
    using BufferingCallback = std::function<void(bool buffering, double ms)>;
    using CoreIdleCallback = std::function<void(bool idle, double ms)>;

    MpvPlayerVk();
    ~MpvPlayerVk();

    bool init(VulkanContext* vk, VideoSurface* subsurface = nullptr);
    void cleanup();
    bool loadFile(const std::string& path, double startSeconds = 0.0);

    // Process pending mpv events (call from main loop)
    void processEvents();

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
    void setSpeed(double speed);

    // State queries
    double getPosition() const;
    double getDuration() const;
    double getSpeed() const;
    bool isPaused() const;
    bool isPlaying() const { return playing_; }

    void setRedrawCallback(RedrawCallback cb) { redraw_callback_ = cb; }
    bool needsRedraw() const { return needs_redraw_.load(); }
    void clearRedrawFlag() { needs_redraw_ = false; }

    // Event callbacks (set these to receive mpv events)
    void setPositionCallback(PositionCallback cb) { on_position_ = cb; }
    void setDurationCallback(DurationCallback cb) { on_duration_ = cb; }
    void setStateCallback(StateCallback cb) { on_state_ = cb; }
    void setPlayingCallback(PlaybackCallback cb) { on_playing_ = cb; }
    void setFinishedCallback(PlaybackCallback cb) { on_finished_ = cb; }
    void setCanceledCallback(PlaybackCallback cb) { on_canceled_ = cb; }
    void setSeekedCallback(SeekCallback cb) { on_seeked_ = cb; }
    void setBufferingCallback(BufferingCallback cb) { on_buffering_ = cb; }
    void setCoreIdleCallback(CoreIdleCallback cb) { on_core_idle_ = cb; }

    bool isHdr() const { return subsurface_ && subsurface_->isHdr(); }
    VideoSurface* subsurface() const { return subsurface_; }

private:
    static void onMpvRedraw(void* ctx);
    static void onMpvWakeup(void* ctx);
    void handleMpvEvent(struct mpv_event* event);

    VulkanContext* vk_ = nullptr;
    VideoSurface* subsurface_ = nullptr;
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ctx_ = nullptr;

    RedrawCallback redraw_callback_;
    PositionCallback on_position_;
    DurationCallback on_duration_;
    StateCallback on_state_;
    PlaybackCallback on_playing_;
    PlaybackCallback on_finished_;
    PlaybackCallback on_canceled_;
    SeekCallback on_seeked_;
    BufferingCallback on_buffering_;
    CoreIdleCallback on_core_idle_;

    std::atomic<bool> needs_redraw_{false};
    std::atomic<bool> has_events_{false};
    bool playing_ = false;
    bool seeking_ = false;
    double last_position_ = 0.0;
};
