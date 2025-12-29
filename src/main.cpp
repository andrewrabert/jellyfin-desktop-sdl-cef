#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <iostream>
#include <filesystem>
#include <atomic>
#include <vector>
#include <cstring>
#include <mutex>
#include <chrono>
#include <algorithm>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"

#include "vulkan_context.h"
#include "vulkan_compositor.h"
#include "mpv_player_vk.h"
#include "wayland_subsurface.h"
#include "cef_app.h"
#include "cef_client.h"
#include "settings.h"

// Fade constants
constexpr float FADE_DURATION_SEC = 0.3f;
constexpr float IDLE_TIMEOUT_SEC = 5.0f;

// Double/triple click detection
constexpr int MULTI_CLICK_DISTANCE = 4;
constexpr Uint64 MULTI_CLICK_TIME = 500;

// Convert SDL modifier state to CEF modifier flags
int sdlModsToCef(SDL_Keymod sdlMods) {
    int cef = 0;
    if (sdlMods & SDL_KMOD_SHIFT) cef |= (1 << 1);
    if (sdlMods & SDL_KMOD_CTRL)  cef |= (1 << 2);
    if (sdlMods & SDL_KMOD_ALT)   cef |= (1 << 3);
    return cef;
}

int main(int argc, char* argv[]) {
    // Parse CLI args for test video
    std::string test_video;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) {
            test_video = argv[++i];
        }
    }

    // CEF initialization
    CefMainArgs main_args(argc, argv);
    CefRefPtr<App> app(new App());

    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    // SDL initialization with Vulkan
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    const int width = 1280;
    const int height = 720;

    SDL_Window* window = SDL_CreateWindow(
        "Jellyfin Desktop",
        width, height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_StartTextInput(window);

    // Initialize Vulkan
    VulkanContext vk;
    if (!vk.init(window)) {
        std::cerr << "Vulkan init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!vk.createSwapchain(width, height)) {
        std::cerr << "Swapchain creation failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize Wayland subsurface for HDR video
    WaylandSubsurface subsurface;
    if (!subsurface.init(window, vk.instance(), vk.physicalDevice(), vk.device(), vk.queueFamily(),
                         vk.deviceExtensions(), vk.deviceExtensionCount(), vk.features())) {
        std::cerr << "Fatal: Wayland subsurface init failed" << std::endl;
        return 1;
    }
    if (!subsurface.createSwapchain(width, height)) {
        std::cerr << "Fatal: Wayland subsurface swapchain failed" << std::endl;
        return 1;
    }
    bool has_subsurface = true;
    std::cout << "Using Wayland subsurface for video (HDR: "
              << (subsurface.isHdr() ? "yes" : "no") << ")" << std::endl;

    // Initialize mpv player
    MpvPlayerVk mpv;
    bool has_video = false;
    if (!mpv.init(&vk, has_subsurface ? &subsurface : nullptr)) {
        std::cerr << "MpvPlayerVk init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize compositor
    VulkanCompositor compositor;
    if (!compositor.init(&vk, width, height)) {
        std::cerr << "VulkanCompositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Load settings
    Settings::instance().load();

    // CEF settings
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = true;

    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe").parent_path();
    CefString(&settings.resources_dir_path).FromString(exe_path.string());
    CefString(&settings.locales_dir_path).FromString((exe_path / "locales").string());

    // Cache path
    std::filesystem::path cache_path;
#ifdef _WIN32
    if (const char* appdata = std::getenv("LOCALAPPDATA")) {
        cache_path = std::filesystem::path(appdata) / "jellyfin-desktop-cef";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        cache_path = std::filesystem::path(home) / "Library" / "Caches" / "jellyfin-desktop-cef";
    }
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        cache_path = std::filesystem::path(xdg) / "jellyfin-desktop-cef";
    } else if (const char* home = std::getenv("HOME")) {
        cache_path = std::filesystem::path(home) / ".cache" / "jellyfin-desktop-cef";
    }
#endif
    if (!cache_path.empty()) {
        std::filesystem::create_directories(cache_path);
        CefString(&settings.root_cache_path).FromString(cache_path.string());
        CefString(&settings.cache_path).FromString((cache_path / "cache").string());
    }

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        std::cerr << "CefInitialize failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Create browser
    std::atomic<bool> texture_dirty{false};
    std::mutex buffer_mutex;
    std::vector<uint8_t> paint_buffer_copy;
    int paint_width = 0, paint_height = 0;

    // Player command queue
    struct PlayerCmd {
        std::string cmd;
        std::string url;
        int intArg;
    };
    std::mutex cmd_mutex;
    std::vector<PlayerCmd> pending_cmds;

    CefRefPtr<Client> client(new Client(width, height,
        [&](const void* buffer, int w, int h) {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            size_t size = w * h * 4;
            paint_buffer_copy.resize(size);
            memcpy(paint_buffer_copy.data(), buffer, size);
            paint_width = w;
            paint_height = h;
            texture_dirty = true;
        },
        [&](const std::string& cmd, const std::string& arg, int intArg) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_cmds.push_back({cmd, arg, intArg});
        }
    ));

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);

    CefBrowserSettings browser_settings;
    browser_settings.background_color = 0;

    std::string html_path = "file://" + (exe_path / "resources" / "index.html").string();
    std::cout << "Loading: " << html_path << std::endl;

    CefBrowserHost::CreateBrowser(window_info, client, html_path, browser_settings, nullptr, nullptr);

    // State tracking
    using Clock = std::chrono::steady_clock;
    auto last_activity = Clock::now();
    float overlay_alpha = 1.0f;
    int mouse_x = 0, mouse_y = 0;
    Uint64 last_click_time = 0;
    int last_click_x = 0, last_click_y = 0;
    int last_click_button = 0;
    int click_count = 0;
    bool focus_set = false;
    int current_width = width;
    int current_height = height;

    // Create command buffer and sync objects
    VkCommandBuffer cmd_buffer;
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = vk.commandPool();
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk.device(), &alloc_info, &cmd_buffer);

    VkSemaphore image_available, render_finished;
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(vk.device(), &sem_info, nullptr, &image_available);
    vkCreateSemaphore(vk.device(), &sem_info, nullptr, &render_finished);

    VkFence in_flight;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(vk.device(), &fence_info, nullptr, &in_flight);

    // Auto-load test video if provided via --video
    if (!test_video.empty()) {
        std::cout << "[TEST] Loading video: " << test_video << std::endl;
        if (mpv.loadFile(test_video)) {
            has_video = true;
            if (has_subsurface && subsurface.isHdr()) {
                subsurface.setColorspace();
            }
        } else {
            std::cerr << "[TEST] Failed to load: " << test_video << std::endl;
        }
    }

    // Main loop
    bool running = true;
    while (running && !client->isClosed()) {
        auto now = Clock::now();
        bool activity_this_frame = false;

        if (!focus_set) {
            client->sendFocus(true);
            focus_set = true;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) running = false;

            if (event.type == SDL_EVENT_MOUSE_MOTION ||
                event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                event.type == SDL_EVENT_MOUSE_WHEEL ||
                event.type == SDL_EVENT_KEY_DOWN ||
                event.type == SDL_EVENT_KEY_UP) {
                activity_this_frame = true;
            }

            int mods = sdlModsToCef(SDL_GetModState());

            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                mouse_x = static_cast<int>(event.motion.x);
                mouse_y = static_cast<int>(event.motion.y);
                client->sendMouseMove(mouse_x, mouse_y, mods);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                int x = static_cast<int>(event.button.x);
                int y = static_cast<int>(event.button.y);
                int btn = event.button.button;
                Uint64 now_ticks = SDL_GetTicks();

                int dx = x - last_click_x;
                int dy = y - last_click_y;
                bool same_spot = (dx * dx + dy * dy) <= (MULTI_CLICK_DISTANCE * MULTI_CLICK_DISTANCE);
                bool same_button = (btn == last_click_button);
                bool in_time = (now_ticks - last_click_time) <= MULTI_CLICK_TIME;

                if (same_spot && same_button && in_time) {
                    click_count = (click_count % 3) + 1;
                } else {
                    click_count = 1;
                }

                last_click_time = now_ticks;
                last_click_x = x;
                last_click_y = y;
                last_click_button = btn;

                client->sendFocus(true);
                client->sendMouseClick(x, y, true, btn, click_count, mods);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                client->sendMouseClick(static_cast<int>(event.button.x), static_cast<int>(event.button.y),
                                       false, event.button.button, click_count, mods);
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                client->sendMouseWheel(mouse_x, mouse_y, event.wheel.x, event.wheel.y, mods);
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                client->sendFocus(true);
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                client->sendFocus(false);
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                current_width = event.window.data1;
                current_height = event.window.data2;
                vkDeviceWaitIdle(vk.device());
                vk.recreateSwapchain(current_width, current_height);
                compositor.resize(current_width, current_height);
                client->resize(current_width, current_height);
                if (has_subsurface) {
                    vkDeviceWaitIdle(subsurface.vkDevice());
                    subsurface.recreateSwapchain(current_width, current_height);
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                bool down = (event.type == SDL_EVENT_KEY_DOWN);
                SDL_Keycode key = event.key.key;
                bool is_control_key = (key == SDLK_BACKSPACE || key == SDLK_DELETE ||
                    key == SDLK_RETURN || key == SDLK_ESCAPE || key == SDLK_SPACE ||
                    key == SDLK_TAB || key == SDLK_LEFT || key == SDLK_RIGHT ||
                    key == SDLK_UP || key == SDLK_DOWN || key == SDLK_HOME ||
                    key == SDLK_END || key == SDLK_PAGEUP || key == SDLK_PAGEDOWN ||
                    key == SDLK_F5 || key == SDLK_F11);
                bool has_ctrl = (mods & (1 << 2)) != 0;
                if (is_control_key || has_ctrl) {
                    client->sendKeyEvent(key, down, mods);
                }
            } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                for (const char* c = event.text.text; *c; ++c) {
                    client->sendChar(static_cast<unsigned char>(*c), mods);
                }
            }
        }

        if (activity_this_frame) {
            last_activity = now;
        }

        // Process player commands
        {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            for (const auto& cmd : pending_cmds) {
                if (cmd.cmd == "load") {
                    std::cout << "[MAIN] playerLoad: " << cmd.url << std::endl;
                    if (mpv.loadFile(cmd.url)) {
                        has_video = true;
                        if (has_subsurface && subsurface.isHdr()) {
                            subsurface.setColorspace();
                        }
                        client->emitPlaying();
                    } else {
                        client->emitError("Failed to load video");
                    }
                } else if (cmd.cmd == "stop") {
                    mpv.stop();
                    has_video = false;
                    client->emitFinished();
                } else if (cmd.cmd == "pause") {
                    mpv.pause();
                    client->emitPaused();
                } else if (cmd.cmd == "play") {
                    mpv.play();
                    client->emitPlaying();
                } else if (cmd.cmd == "seek") {
                    mpv.seek(static_cast<double>(cmd.intArg) / 1000.0);
                } else if (cmd.cmd == "volume") {
                    mpv.setVolume(cmd.intArg);
                } else if (cmd.cmd == "mute") {
                    mpv.setMuted(cmd.intArg != 0);
                } else if (cmd.cmd == "fullscreen") {
                    bool enable = cmd.intArg != 0;
                    SDL_SetWindowFullscreen(window, enable);
                }
            }
            pending_cmds.clear();
        }

        // Calculate fade
        float idle_sec = std::chrono::duration<float>(now - last_activity).count();
        if (idle_sec < IDLE_TIMEOUT_SEC) {
            overlay_alpha = 1.0f;
        } else {
            float fade_progress = (idle_sec - IDLE_TIMEOUT_SEC) / FADE_DURATION_SEC;
            overlay_alpha = std::max(0.0f, 1.0f - fade_progress);
        }

        CefDoMessageLoopWork();

        // Update position/duration
        static auto last_position_update = Clock::now();
        if (has_video && mpv.isPlaying()) {
            auto time_since_update = std::chrono::duration<float>(now - last_position_update).count();
            if (time_since_update >= 0.5f) {
                double pos = mpv.getPosition() * 1000.0;
                double dur = mpv.getDuration() * 1000.0;
                client->updatePosition(pos);
                if (dur > 0) {
                    client->updateDuration(dur);
                }
                last_position_update = now;
            }
        }

        // Update overlay texture
        if (texture_dirty) {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            if (!paint_buffer_copy.empty()) {
                compositor.updateOverlay(paint_buffer_copy.data(), paint_width, paint_height);
            }
            texture_dirty = false;
        }

        // Wait for previous frame
        vkWaitForFences(vk.device(), 1, &in_flight, VK_TRUE, UINT64_MAX);
        vkResetFences(vk.device(), 1, &in_flight);

        // Acquire swapchain image
        uint32_t image_idx;
        VkResult result = vkAcquireNextImageKHR(vk.device(), vk.swapchain(), UINT64_MAX,
                                                 image_available, VK_NULL_HANDLE, &image_idx);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            std::cerr << "Main swapchain out of date, recreating..." << std::endl;
            SDL_GetWindowSize(window, &current_width, &current_height);
            vkDeviceWaitIdle(vk.device());
            vk.recreateSwapchain(current_width, current_height);
            compositor.resize(current_width, current_height);
            continue;
        }

        // Begin command buffer
        vkResetCommandBuffer(cmd_buffer, 0);
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd_buffer, &begin_info);

        // Render video to subsurface (if available) or main swapchain
        if (has_video && has_subsurface && mpv.hasFrame()) {
            // mpv renders to separate HDR subsurface (using libplacebo swapchain)
            VkImage sub_image;
            VkImageView sub_view;
            VkFormat sub_format;
            if (subsurface.startFrame(&sub_image, &sub_view, &sub_format)) {
                mpv.render(sub_image, sub_view,
                          subsurface.width(), subsurface.height(),
                          sub_format);
                subsurface.submitFrame();
            }
        } else if (has_video && !has_subsurface) {
            // Fallback: mpv renders to main swapchain (SDR)
            mpv.render(vk.swapchainImages()[image_idx], vk.swapchainViews()[image_idx],
                      vk.swapchainExtent().width, vk.swapchainExtent().height, vk.swapchainFormat());
        }

        // Clear main swapchain if using subsurface for video or no video
        if (!has_video || has_subsurface) {
            // Clear to black when no video
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = vk.swapchainImages()[image_idx];
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            // Transparent when using subsurface (so video shows through), opaque black otherwise
            float alpha = (has_video && has_subsurface) ? 0.0f : 1.0f;
            VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, alpha}};
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd_buffer, vk.swapchainImages()[image_idx],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &range);

            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = 0;

            vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // Composite overlay (skip when using --video for HDR testing)
        if (test_video.empty()) {
            float alpha = has_video ? overlay_alpha : 1.0f;
            compositor.composite(cmd_buffer, vk.swapchainImages()[image_idx], vk.swapchainViews()[image_idx],
                                vk.swapchainExtent().width, vk.swapchainExtent().height, alpha);
        }

        vkEndCommandBuffer(cmd_buffer);

        // Submit
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &image_available;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmd_buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_finished;

        vkQueueSubmit(vk.queue(), 1, &submit_info, in_flight);

        // Present
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_finished;
        VkSwapchainKHR swapchain = vk.swapchain();
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_idx;

        result = vkQueuePresentKHR(vk.queue(), &present_info);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            std::cerr << "Present returned " << result << ", recreating swapchain" << std::endl;
            SDL_GetWindowSize(window, &current_width, &current_height);
            vkDeviceWaitIdle(vk.device());
            vk.recreateSwapchain(current_width, current_height);
            compositor.resize(current_width, current_height);
        }
    }

    // Cleanup - order matters: wait for GPU, destroy resources, then window
    vkDeviceWaitIdle(vk.device());
    vkDestroySemaphore(vk.device(), image_available, nullptr);
    vkDestroySemaphore(vk.device(), render_finished, nullptr);
    vkDestroyFence(vk.device(), in_flight, nullptr);

    mpv.cleanup();
    compositor.cleanup();
    if (has_subsurface) {
        subsurface.cleanup();
    }
    vk.cleanup();

    CefShutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
