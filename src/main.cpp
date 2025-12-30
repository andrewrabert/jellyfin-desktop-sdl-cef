#include <SDL3/SDL.h>
#include <iostream>
#include <filesystem>
#include <atomic>
#include <vector>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <algorithm>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include "cgl_context.h"
#include "macos_layer.h"
#else
#include "egl_context.h"
#include "wayland_subsurface.h"
#endif

#include "opengl_compositor.h"
#include "mpv_player_vk.h"
#include "cef_app.h"
#include "cef_client.h"
#include "menu_overlay.h"
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

static auto _main_start = std::chrono::steady_clock::now();
inline long _ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _main_start).count(); }

int main(int argc, char* argv[]) {
    // Parse CLI args
    std::string test_video;
    bool use_gpu_overlay = false;  // DMA-BUF shared textures (experimental)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) {
            test_video = argv[++i];
        } else if (strcmp(argv[i], "--gpu-overlay") == 0) {
            use_gpu_overlay = true;
        }
    }

    // CEF initialization
    CefMainArgs main_args(argc, argv);
    CefRefPtr<App> app(new App());

    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    // SDL initialization with OpenGL (for main surface CEF overlay)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    const int width = 1280;
    const int height = 720;

    // Use plain Wayland window - we create our own EGL context
    SDL_Window* window = SDL_CreateWindow(
        "Jellyfin Desktop",
        width, height,
        SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_StartTextInput(window);

#ifdef __APPLE__
    // macOS: Initialize CGL context for OpenGL rendering
    CGLContext glctx;
    if (!glctx.init(window)) {
        std::cerr << "CGL init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize macOS layer for HDR video (uses MoltenVK)
    MacOSVideoLayer videoLayer;
    if (!videoLayer.init(window, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0,
                         nullptr, 0, nullptr)) {
        std::cerr << "Fatal: macOS video layer init failed" << std::endl;
        return 1;
    }
    if (!videoLayer.createSwapchain(width, height)) {
        std::cerr << "Fatal: macOS video layer swapchain failed" << std::endl;
        return 1;
    }
    bool has_subsurface = true;
    std::cout << "Using macOS CAMetalLayer for video (HDR: "
              << (videoLayer.isHdr() ? "yes" : "no") << ")" << std::endl;

    // Initialize mpv player (using video layer's Vulkan context)
    MpvPlayerVk mpv;
    bool has_video = false;
    if (!mpv.init(nullptr, &videoLayer)) {
        std::cerr << "MpvPlayerVk init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize OpenGL compositor for CEF overlay
    OpenGLCompositor compositor;
    if (!compositor.init(&glctx, width, height, use_gpu_overlay)) {
        std::cerr << "OpenGLCompositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
#else
    // Linux: Initialize EGL context for OpenGL rendering
    EGLContext_ egl;
    if (!egl.init(window)) {
        std::cerr << "EGL init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize Wayland subsurface for HDR video (uses its own Vulkan)
    WaylandSubsurface subsurface;
    if (!subsurface.init(window, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0,
                         nullptr, 0, nullptr)) {
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

    // Initialize mpv player (using subsurface's Vulkan context)
    MpvPlayerVk mpv;
    bool has_video = false;
    if (!mpv.init(nullptr, &subsurface)) {
        std::cerr << "MpvPlayerVk init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize OpenGL compositor for CEF overlay
    OpenGLCompositor compositor;
    if (!compositor.init(&egl, width, height, use_gpu_overlay)) {
        std::cerr << "OpenGLCompositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
#endif

    // Load settings
    Settings::instance().load();

    // CEF settings
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = true;

#ifdef __APPLE__
    // macOS: use _NSGetExecutablePath
    char exe_buf[PATH_MAX];
    uint32_t exe_size = sizeof(exe_buf);
    std::filesystem::path exe_path;
    if (_NSGetExecutablePath(exe_buf, &exe_size) == 0) {
        exe_path = std::filesystem::canonical(exe_buf).parent_path();
    } else {
        exe_path = std::filesystem::current_path();
    }
#else
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
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

    // Set GPU overlay mode before CefInitialize (affects command line processing)
    App::SetGpuOverlayEnabled(use_gpu_overlay);
    std::cout << "CEF rendering: " << (use_gpu_overlay ? "GPU (DMA-BUF)" : "software") << std::endl;

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        std::cerr << "CefInitialize failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Create browser
    std::mutex buffer_mutex;
    int paint_width = 0, paint_height = 0;

    // Player command queue
    struct PlayerCmd {
        std::string cmd;
        std::string url;
        int intArg;
    };
    std::mutex cmd_mutex;
    std::vector<PlayerCmd> pending_cmds;

    // Context menu overlay
    MenuOverlay menu;
    if (!menu.init()) {
        std::cerr << "Warning: Failed to init menu overlay (no font found)" << std::endl;
    }

    CefRefPtr<Client> client(new Client(width, height,
        [&](const void* buffer, int w, int h) {
            // Copy directly to compositor staging buffer (single memcpy)
            std::lock_guard<std::mutex> lock(buffer_mutex);
            void* staging = compositor.getStagingBuffer(w, h);
            if (staging) {
                memcpy(staging, buffer, w * h * 4);
                compositor.markStagingDirty();
            }
            paint_width = w;
            paint_height = h;
        },
        [&](const std::string& cmd, const std::string& arg, int intArg) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_cmds.push_back({cmd, arg, intArg});
        },
#ifdef __APPLE__
        [&](IOSurfaceRef surface, int w, int h) {
            // GPU accelerated paint - queue IOSurface for import in render loop
            compositor.queueIOSurface(surface, w, h);
        },
#else
        [&](const AcceleratedPaintInfo& info) {
            // GPU accelerated paint - queue DMA-BUF for import in render loop
            compositor.queueDmaBuf(info);
        },
#endif
        &menu
    ));

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
#if defined(CEF_X11) || defined(__linux__)
    window_info.shared_texture_enabled = use_gpu_overlay;
#endif

    CefBrowserSettings browser_settings;
    browser_settings.background_color = 0;
    // Match CEF frame rate to display refresh rate
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    if (mode && mode->refresh_rate > 0) {
        browser_settings.windowless_frame_rate = static_cast<int>(mode->refresh_rate);
        std::cout << "CEF frame rate: " << mode->refresh_rate << " Hz" << std::endl;
    } else {
        browser_settings.windowless_frame_rate = 60;
    }

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

    // Auto-load test video if provided via --video
    if (!test_video.empty()) {
        std::cout << "[TEST] Loading video: " << test_video << std::endl;
        if (mpv.loadFile(test_video)) {
            has_video = true;
            if (has_subsurface && subsurface.isHdr()) {
                subsurface.setColorspace();
            }
            client->emitPlaying();
        } else {
            std::cerr << "[TEST] Failed to load: " << test_video << std::endl;
        }
    }

    // Main loop - simplified (no Vulkan command buffers for main surface)
    bool running = true;
    bool needs_render = true;  // Render first frame
    while (running && !client->isClosed()) {
        auto now = Clock::now();
        bool activity_this_frame = false;

        if (!focus_set) {
            client->sendFocus(true);
            focus_set = true;
        }

        // Event-driven: wait for events when idle, poll when active
        SDL_Event event;
        bool have_event;
        if (needs_render || has_video || compositor.hasPendingContent() || compositor.hasPendingDmaBuf()) {
            have_event = SDL_PollEvent(&event);
        } else {
            // Short wait - just yield CPU, don't block long (1ms for ~1000Hz max)
            have_event = SDL_WaitEventTimeout(&event, 1);
        }

        while (have_event) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !menu.isOpen()) running = false;

            if (event.type == SDL_EVENT_MOUSE_MOTION ||
                event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                event.type == SDL_EVENT_MOUSE_WHEEL ||
                event.type == SDL_EVENT_KEY_DOWN ||
                event.type == SDL_EVENT_KEY_UP) {
                activity_this_frame = true;
            }

            int mods = sdlModsToCef(SDL_GetModState());

            // Route input to menu overlay if open
            if (menu.isOpen()) {
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    menu.handleMouseMove(static_cast<int>(event.motion.x), static_cast<int>(event.motion.y));
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    menu.handleMouseClick(static_cast<int>(event.button.x), static_cast<int>(event.button.y), true);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    menu.handleMouseClick(static_cast<int>(event.button.x), static_cast<int>(event.button.y), false);
                } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    menu.close();
                }
                continue;
            }

            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                mouse_x = static_cast<int>(event.motion.x);
                mouse_y = static_cast<int>(event.motion.y);
                SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
                if (buttons & SDL_BUTTON_LMASK) mods |= (1 << 5);
                if (buttons & SDL_BUTTON_MMASK) mods |= (1 << 6);
                if (buttons & SDL_BUTTON_RMASK) mods |= (1 << 7);
                client->sendMouseMove(mouse_x, mouse_y, mods);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                int x = static_cast<int>(event.button.x);
                int y = static_cast<int>(event.button.y);
                int btn = event.button.button;
                std::cerr << "[SDL] Button DOWN: " << btn << " at " << x << "," << y << std::endl;
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
                auto resize_start = std::chrono::steady_clock::now();
                current_width = event.window.data1;
                current_height = event.window.data2;

#ifdef __APPLE__
                // Resize CGL context
                glctx.resize(current_width, current_height);
                compositor.resize(current_width, current_height);
                client->resize(current_width, current_height);

                // Resize video layer
                if (has_subsurface) {
                    videoLayer.resize(current_width, current_height);
                }
#else
                // Resize EGL context (handles wl_egl_window resize)
                egl.resize(current_width, current_height);
                compositor.resize(current_width, current_height);
                client->resize(current_width, current_height);

                // Resize subsurface for video
                if (has_subsurface) {
                    vkDeviceWaitIdle(subsurface.vkDevice());
                    subsurface.recreateSwapchain(current_width, current_height);
                }
#endif

                auto resize_end = std::chrono::steady_clock::now();
                std::cerr << "[" << _ms() << "ms] resize: total="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(resize_end-resize_start).count()
                          << "ms" << std::endl;
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
            have_event = SDL_PollEvent(&event);
        }

        // Determine if we need to render this frame
        // With vsync, always render to maintain consistent frame pacing
#ifdef __APPLE__
        needs_render = activity_this_frame || has_video || compositor.hasPendingContent() || compositor.hasPendingIOSurface();
#else
        needs_render = activity_this_frame || has_video || compositor.hasPendingContent() || compositor.hasPendingDmaBuf();
#endif

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
#ifdef __APPLE__
                        if (has_subsurface && videoLayer.isHdr()) {
                            // macOS EDR is automatic
                        }
#else
                        if (has_subsurface && subsurface.isHdr()) {
                            subsurface.setColorspace();
                        }
#endif
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

        // Menu overlay blending
        menu.clearRedraw();

        // Render video to subsurface/layer
#ifdef __APPLE__
        if (has_video && has_subsurface && mpv.hasFrame()) {
            VkImage sub_image;
            VkImageView sub_view;
            VkFormat sub_format;
            if (videoLayer.startFrame(&sub_image, &sub_view, &sub_format)) {
                mpv.render(sub_image, sub_view,
                          videoLayer.width(), videoLayer.height(),
                          sub_format);
                videoLayer.submitFrame();
            }
        }

        // Import queued IOSurface if any (GPU path)
        compositor.importQueuedIOSurface();
#else
        if (has_video && has_subsurface && mpv.hasFrame()) {
            VkImage sub_image;
            VkImageView sub_view;
            VkFormat sub_format;
            if (subsurface.startFrame(&sub_image, &sub_view, &sub_format)) {
                mpv.render(sub_image, sub_view,
                          subsurface.width(), subsurface.height(),
                          sub_format);
                subsurface.submitFrame();
            }
        }

        // Import queued DMA-BUF if any (GPU path)
        compositor.importQueuedDmaBuf();
#endif

        // Flush pending overlay data to GPU texture (software path)
        compositor.flushOverlay();

        // Clear main surface (transparent when video playing, black otherwise)
        float bg_alpha = (has_video && has_subsurface) ? 0.0f : 1.0f;
        glClearColor(0.0f, 0.0f, 0.0f, bg_alpha);
        glClear(GL_COLOR_BUFFER_BIT);

        // Composite CEF overlay (skip when using --video for HDR testing)
        if (test_video.empty() && compositor.hasValidOverlay()) {
            float alpha = has_video ? overlay_alpha : 1.0f;
            compositor.composite(current_width, current_height, alpha);
        }

        // Swap buffers
#ifdef __APPLE__
        glctx.swapBuffers();
#else
        egl.swapBuffers();
#endif
    }

    // Cleanup
    mpv.cleanup();
    compositor.cleanup();
#ifdef __APPLE__
    if (has_subsurface) {
        videoLayer.cleanup();
    }
    glctx.cleanup();
#else
    if (has_subsurface) {
        subsurface.cleanup();
    }
    egl.cleanup();
#endif

    CefShutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
