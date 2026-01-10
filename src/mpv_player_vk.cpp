#include "mpv_player_vk.h"
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_vk.h>
#include <iostream>
#include <clocale>
#include <cmath>

MpvPlayerVk::MpvPlayerVk() = default;

MpvPlayerVk::~MpvPlayerVk() {
    cleanup();
}

void MpvPlayerVk::cleanup() {
    if (render_ctx_) {
        mpv_render_context_free(render_ctx_);
        render_ctx_ = nullptr;
    }
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
}

void MpvPlayerVk::onMpvRedraw(void* ctx) {
    MpvPlayerVk* player = static_cast<MpvPlayerVk*>(ctx);
    player->needs_redraw_ = true;
    if (player->redraw_callback_) {
        player->redraw_callback_();
    }
}

void MpvPlayerVk::onMpvWakeup(void* ctx) {
    MpvPlayerVk* player = static_cast<MpvPlayerVk*>(ctx);
    player->has_events_ = true;
}

void MpvPlayerVk::processEvents() {
    if (!mpv_ || !has_events_.exchange(false)) return;

    while (true) {
        mpv_event* event = mpv_wait_event(mpv_, 0);
        if (event->event_id == MPV_EVENT_NONE) break;
        handleMpvEvent(event);
    }
}

void MpvPlayerVk::handleMpvEvent(mpv_event* event) {
    switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            mpv_event_property* prop = static_cast<mpv_event_property*>(event->data);
            if (strcmp(prop->name, "playback-time") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                double pos = *static_cast<double*>(prop->data);
                // Filter jitter (15ms threshold like jellyfin-desktop)
                if (std::fabs(pos - last_position_) > 0.015) {
                    last_position_ = pos;
                    if (on_position_) on_position_(pos * 1000.0);
                }
            } else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                double dur = *static_cast<double*>(prop->data);
                if (on_duration_) on_duration_(dur * 1000.0);
            } else if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool paused = *static_cast<int*>(prop->data) != 0;
                if (on_state_) on_state_(paused);
            }
            break;
        }
        case MPV_EVENT_START_FILE:
            playing_ = true;
            break;
        case MPV_EVENT_FILE_LOADED:
            if (on_playing_) on_playing_();
            break;
        case MPV_EVENT_END_FILE: {
            mpv_event_end_file* ef = static_cast<mpv_event_end_file*>(event->data);
            playing_ = false;
            if (on_finished_) on_finished_();
            break;
        }
        default:
            break;
    }
}

bool MpvPlayerVk::init(VulkanContext* vk, VideoSurface* subsurface) {
    vk_ = vk;
    subsurface_ = subsurface;

    std::setlocale(LC_NUMERIC, "C");

    mpv_ = mpv_create();
    if (!mpv_) {
        std::cerr << "mpv_create failed" << std::endl;
        return false;
    }

    mpv_set_option_string(mpv_, "vo", "libmpv");
    mpv_set_option_string(mpv_, "hwdec", "no");  // Force software decode for yuv420p10
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "terminal", "no");
    mpv_set_option_string(mpv_, "video-sync", "audio");  // Simple audio sync, no frame interpolation
    mpv_set_option_string(mpv_, "interpolation", "no");  // Disable motion interpolation

    // HDR output configuration
    bool use_hdr = subsurface_ && subsurface_->isHdr();
    if (use_hdr) {
#ifdef __APPLE__
        // macOS EDR uses extended linear sRGB - output linear light values
        mpv_set_option_string(mpv_, "target-prim", "bt.709");
        mpv_set_option_string(mpv_, "target-trc", "linear");
        mpv_set_option_string(mpv_, "tone-mapping", "clip");
        double peak = 1000.0;  // EDR headroom
        mpv_set_option(mpv_, "target-peak", MPV_FORMAT_DOUBLE, &peak);
        std::cerr << "mpv HDR output enabled (bt.709/linear for macOS EDR)" << std::endl;
#else
        // Linux Wayland HDR uses PQ/BT.2020
        mpv_set_option_string(mpv_, "target-prim", "bt.2020");
        mpv_set_option_string(mpv_, "target-trc", "pq");
        mpv_set_option_string(mpv_, "target-colorspace-hint", "yes");
        mpv_set_option_string(mpv_, "tone-mapping", "clip");  // No tone mapping for passthrough
        double peak = 1000.0;
        mpv_set_option(mpv_, "target-peak", MPV_FORMAT_DOUBLE, &peak);
        std::cerr << "mpv HDR output enabled (bt.2020/pq/1000 nits)" << std::endl;
#endif
    }

    if (mpv_initialize(mpv_) < 0) {
        std::cerr << "mpv_initialize failed" << std::endl;
        return false;
    }

    // Set up property observation (like jellyfin-desktop)
    mpv_observe_property(mpv_, 0, "playback-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);

    // Wakeup callback for event-driven processing
    mpv_set_wakeup_callback(mpv_, onMpvWakeup, this);

    // Set up Vulkan render context - use subsurface's device if available for HDR
    mpv_vulkan_init_params vk_params{};
    if (subsurface_ && subsurface_->vkDevice()) {
        // Use subsurface's Vulkan device for HDR rendering
        vk_params.instance = subsurface_->vkInstance();
        vk_params.physical_device = subsurface_->vkPhysicalDevice();
        vk_params.device = subsurface_->vkDevice();
        vk_params.graphics_queue = subsurface_->vkQueue();
        vk_params.graphics_queue_family = subsurface_->vkQueueFamily();
        vk_params.get_instance_proc_addr = subsurface_->vkGetProcAddr();
        // Use subsurface's features/extensions (same device that was created)
        vk_params.features = subsurface_->features();
        vk_params.extensions = subsurface_->deviceExtensions();
        vk_params.num_extensions = subsurface_->deviceExtensionCount();
        std::cerr << "mpv using subsurface's Vulkan device for HDR" << std::endl;
    } else {
        // Use main VulkanContext
        vk_params.instance = vk_->instance();
        vk_params.physical_device = vk_->physicalDevice();
        vk_params.device = vk_->device();
        vk_params.graphics_queue = vk_->queue();
        vk_params.graphics_queue_family = vk_->queueFamily();
        vk_params.get_instance_proc_addr = vkGetInstanceProcAddr;
        vk_params.features = vk_->features();
        vk_params.extensions = vk_->deviceExtensions();
        vk_params.num_extensions = vk_->deviceExtensionCount();
    }

    int advanced_control = 1;

    // Try gpu-next first (libplacebo), fall back to gpu (legacy) if needed
    const char* backends[] = {"gpu-next", "gpu"};
    int result = -1;

    for (const char* backend : backends) {
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_VULKAN)},
            {MPV_RENDER_PARAM_BACKEND, const_cast<char*>(backend)},
            {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vk_params},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };

        result = mpv_render_context_create(&render_ctx_, mpv_, params);
        if (result >= 0) {
            std::cerr << "mpv using backend: " << backend << std::endl;
            break;
        }
        std::cerr << "mpv backend '" << backend << "' failed: " << mpv_error_string(result) << std::endl;
    }

    if (result < 0) {
        std::cerr << "mpv_render_context_create failed (all backends)" << std::endl;
        return false;
    }

    mpv_render_context_set_update_callback(render_ctx_, onMpvRedraw, this);

    std::cerr << "mpv Vulkan render context created" << std::endl;
    return true;
}

bool MpvPlayerVk::loadFile(const std::string& path, double startSeconds) {
    // Set start position before loading (mpv uses this for the next file)
    if (startSeconds > 0.0) {
        std::string startStr = std::to_string(startSeconds);
        mpv_set_option_string(mpv_, "start", startStr.c_str());
    } else {
        mpv_set_option_string(mpv_, "start", "0");
    }

    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    int ret = mpv_command(mpv_, cmd);
    if (ret >= 0) {
        playing_ = true;
    }
    return ret >= 0;
}

void MpvPlayerVk::stop() {
    if (!mpv_) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command(mpv_, cmd);
    playing_ = false;
}

void MpvPlayerVk::pause() {
    if (!mpv_) return;
    int pause = 1;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
}

void MpvPlayerVk::play() {
    if (!mpv_) return;
    int pause = 0;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
}

void MpvPlayerVk::seek(double seconds) {
    if (!mpv_) return;
    std::string time_str = std::to_string(seconds);
    const char* cmd[] = {"seek", time_str.c_str(), "absolute", nullptr};
    mpv_command(mpv_, cmd);
}

void MpvPlayerVk::setVolume(int volume) {
    if (!mpv_) return;
    double vol = static_cast<double>(volume);
    mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void MpvPlayerVk::setMuted(bool muted) {
    if (!mpv_) return;
    int m = muted ? 1 : 0;
    mpv_set_property(mpv_, "mute", MPV_FORMAT_FLAG, &m);
}

void MpvPlayerVk::setSpeed(double speed) {
    if (!mpv_) return;
    mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
}

double MpvPlayerVk::getPosition() const {
    if (!mpv_) return 0;
    double pos = 0;
    mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

double MpvPlayerVk::getDuration() const {
    if (!mpv_) return 0;
    double dur = 0;
    mpv_get_property(mpv_, "duration", MPV_FORMAT_DOUBLE, &dur);
    return dur;
}

bool MpvPlayerVk::isPaused() const {
    if (!mpv_) return false;
    int paused = 0;
    mpv_get_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused);
    return paused != 0;
}

bool MpvPlayerVk::hasFrame() const {
    if (!render_ctx_) return false;
    uint64_t flags = mpv_render_context_update(render_ctx_);
    return (flags & MPV_RENDER_UPDATE_FRAME) != 0;
}

void MpvPlayerVk::render(VkImage image, VkImageView view, uint32_t width, uint32_t height, VkFormat format) {
    if (!render_ctx_) return;

    mpv_vulkan_fbo fbo{};
    fbo.image = image;
    fbo.image_view = view;
    fbo.width = width;
    fbo.height = height;
    fbo.format = format;
    fbo.current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    fbo.target_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    int flip_y = 0;
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_VULKAN_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    mpv_render_context_render(render_ctx_, render_params);
}
