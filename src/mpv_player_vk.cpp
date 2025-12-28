#include "mpv_player_vk.h"
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_vk.h>
#include <iostream>
#include <clocale>

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

bool MpvPlayerVk::init(VulkanContext* vk, WaylandSubsurface* subsurface) {
    vk_ = vk;
    subsurface_ = subsurface;

    std::setlocale(LC_NUMERIC, "C");

    mpv_ = mpv_create();
    if (!mpv_) {
        std::cerr << "mpv_create failed" << std::endl;
        return false;
    }

    mpv_set_option_string(mpv_, "vo", "libmpv");
    mpv_set_option_string(mpv_, "hwdec", "auto");
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "terminal", "yes");
    mpv_set_option_string(mpv_, "msg-level", "all=v");

    // HDR output configuration (before initialize for mpv options)
    bool use_hdr = subsurface_ && subsurface_->isHdr();
    if (use_hdr) {
        mpv_set_option_string(mpv_, "target-prim", "bt.2020");
        mpv_set_option_string(mpv_, "target-trc", "pq");
        double peak = 1000.0;
        mpv_set_option(mpv_, "target-peak", MPV_FORMAT_DOUBLE, &peak);
        std::cout << "mpv HDR output enabled (bt.2020/pq/1000 nits)" << std::endl;
    }

    if (mpv_initialize(mpv_) < 0) {
        std::cerr << "mpv_initialize failed" << std::endl;
        return false;
    }

    // Set up Vulkan render context
    mpv_vulkan_init_params vk_params{};
    vk_params.instance = vk_->instance();
    vk_params.physical_device = vk_->physicalDevice();
    vk_params.device = vk_->device();
    vk_params.graphics_queue = vk_->queue();
    vk_params.graphics_queue_family = vk_->queueFamily();
    vk_params.get_instance_proc_addr = vkGetInstanceProcAddr;
    vk_params.features = vk_->features();
    vk_params.extensions = vk_->deviceExtensions();
    vk_params.num_extensions = vk_->deviceExtensionCount();

    int advanced_control = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_VULKAN)},
        {MPV_RENDER_PARAM_BACKEND, const_cast<char*>("gpu-next")},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vk_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    int result = mpv_render_context_create(&render_ctx_, mpv_, params);
    if (result < 0) {
        std::cerr << "mpv_render_context_create failed: " << mpv_error_string(result) << std::endl;
        return false;
    }

    mpv_render_context_set_update_callback(render_ctx_, onMpvRedraw, this);

    std::cout << "mpv Vulkan render context created" << std::endl;
    return true;
}

bool MpvPlayerVk::loadFile(const std::string& path) {
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

void MpvPlayerVk::render(VkImage image, VkImageView view, uint32_t width, uint32_t height, VkFormat format) {
    if (!render_ctx_) return;

    uint64_t flags = mpv_render_context_update(render_ctx_);
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) {
        return;
    }

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
