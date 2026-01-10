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
#include "include/wrapper/cef_library_loader.h"
#include "include/cef_application_mac.h"

// Initialize CEF-compatible NSApplication before SDL
void initMacApplication();
// Activate window for keyboard focus after SDL window creation
void activateMacWindow();
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include "macos_layer.h"
#include "metal_compositor.h"
#else
#include "egl_context.h"
#include "wayland_subsurface.h"
#include "opengl_compositor.h"
#endif
#include "mpv_player_vk.h"
#include "cef_app.h"
#include "cef_client.h"
#include "menu_overlay.h"
#include "settings.h"

// Fade constants
constexpr float FADE_DURATION_SEC = 0.3f;
constexpr float IDLE_TIMEOUT_SEC = 5.0f;

// Overlay fade constants
constexpr float OVERLAY_FADE_DELAY_SEC = 1.0f;
constexpr float OVERLAY_FADE_DURATION_SEC = 0.25f;

// Double/triple click detection
constexpr int MULTI_CLICK_DISTANCE = 4;
constexpr Uint64 MULTI_CLICK_TIME = 500;

// Convert SDL modifier state to CEF modifier flags
// CEF flags: SHIFT=1<<1, CTRL=1<<2, ALT=1<<3, CMD=1<<7
int sdlModsToCef(SDL_Keymod sdlMods) {
    int cef = 0;
    if (sdlMods & SDL_KMOD_SHIFT) cef |= (1 << 1);  // EVENTFLAG_SHIFT_DOWN
    if (sdlMods & SDL_KMOD_CTRL)  cef |= (1 << 2);  // EVENTFLAG_CONTROL_DOWN
    if (sdlMods & SDL_KMOD_ALT)   cef |= (1 << 3);  // EVENTFLAG_ALT_DOWN
#ifdef __APPLE__
    if (sdlMods & SDL_KMOD_GUI)   cef |= (1 << 7);  // EVENTFLAG_COMMAND_DOWN (Cmd key)
#endif
    return cef;
}

// Map CEF cursor type to SDL system cursor
SDL_SystemCursor cefCursorToSDL(cef_cursor_type_t type) {
    switch (type) {
        case CT_POINTER: return SDL_SYSTEM_CURSOR_DEFAULT;
        case CT_CROSS: return SDL_SYSTEM_CURSOR_CROSSHAIR;
        case CT_HAND: return SDL_SYSTEM_CURSOR_POINTER;
        case CT_IBEAM: return SDL_SYSTEM_CURSOR_TEXT;
        case CT_WAIT: return SDL_SYSTEM_CURSOR_WAIT;
        case CT_HELP: return SDL_SYSTEM_CURSOR_DEFAULT;  // No help cursor in SDL
        case CT_EASTRESIZE: return SDL_SYSTEM_CURSOR_E_RESIZE;
        case CT_NORTHRESIZE: return SDL_SYSTEM_CURSOR_N_RESIZE;
        case CT_NORTHEASTRESIZE: return SDL_SYSTEM_CURSOR_NE_RESIZE;
        case CT_NORTHWESTRESIZE: return SDL_SYSTEM_CURSOR_NW_RESIZE;
        case CT_SOUTHRESIZE: return SDL_SYSTEM_CURSOR_S_RESIZE;
        case CT_SOUTHEASTRESIZE: return SDL_SYSTEM_CURSOR_SE_RESIZE;
        case CT_SOUTHWESTRESIZE: return SDL_SYSTEM_CURSOR_SW_RESIZE;
        case CT_WESTRESIZE: return SDL_SYSTEM_CURSOR_W_RESIZE;
        case CT_NORTHSOUTHRESIZE: return SDL_SYSTEM_CURSOR_NS_RESIZE;
        case CT_EASTWESTRESIZE: return SDL_SYSTEM_CURSOR_EW_RESIZE;
        case CT_NORTHEASTSOUTHWESTRESIZE: return SDL_SYSTEM_CURSOR_NESW_RESIZE;
        case CT_NORTHWESTSOUTHEASTRESIZE: return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
        case CT_COLUMNRESIZE: return SDL_SYSTEM_CURSOR_EW_RESIZE;
        case CT_ROWRESIZE: return SDL_SYSTEM_CURSOR_NS_RESIZE;
        case CT_MOVE: return SDL_SYSTEM_CURSOR_MOVE;
        case CT_PROGRESS: return SDL_SYSTEM_CURSOR_PROGRESS;
        case CT_NODROP: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
        case CT_NOTALLOWED: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
        case CT_GRAB: return SDL_SYSTEM_CURSOR_POINTER;
        case CT_GRABBING: return SDL_SYSTEM_CURSOR_POINTER;
        default: return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

static auto _main_start = std::chrono::steady_clock::now();
inline long _ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _main_start).count(); }

int main(int argc, char* argv[]) {
#ifdef __APPLE__
    // macOS: Get executable path early for CEF framework loading
    char exe_buf[PATH_MAX];
    uint32_t exe_size = sizeof(exe_buf);
    std::filesystem::path exe_path;
    if (_NSGetExecutablePath(exe_buf, &exe_size) == 0) {
        exe_path = std::filesystem::canonical(exe_buf).parent_path();
    } else {
        exe_path = std::filesystem::current_path();
    }

    // macOS: Load CEF framework dynamically (required - linking alone isn't enough)
    // Framework is at Frameworks/ relative to executable (install_name fixed at build time)
    std::string framework_lib = (exe_path / "Frameworks" /
                                 "Chromium Embedded Framework.framework" /
                                 "Chromium Embedded Framework").string();
    std::cerr << "Loading CEF from: " << framework_lib << std::endl;
    if (!cef_load_library(framework_lib.c_str())) {
        std::cerr << "Failed to load CEF framework from: " << framework_lib << std::endl;
        return 1;
    }
    std::cerr << "CEF framework loaded" << std::endl;

    // CRITICAL: Initialize CEF-compatible NSApplication BEFORE CefExecuteProcess
    // This must happen before any CEF code that might create an NSApplication
    initMacApplication();
#endif

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

    std::cerr << "Calling CefExecuteProcess..." << std::endl;
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    std::cerr << "CefExecuteProcess returned: " << exit_code << std::endl;
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
    // Ensure window has keyboard focus after creation
    activateMacWindow();
#endif

#ifdef __APPLE__
    // macOS: Initialize video layer first (will be at back)
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
    std::cerr << "Using macOS CAMetalLayer for video (HDR: "
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

    // Initialize Metal compositor for CEF overlay (renders on top of video)
    MetalCompositor compositor;
    if (!compositor.init(window, width, height)) {
        std::cerr << "MetalCompositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Second compositor for overlay browser
    MetalCompositor overlay_compositor;
    if (!overlay_compositor.init(window, width, height)) {
        std::cerr << "Overlay compositor init failed" << std::endl;
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
    std::cerr << "Using Wayland subsurface for video (HDR: "
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

    // Second compositor for overlay browser
    OpenGLCompositor overlay_compositor;
    if (!overlay_compositor.init(&egl, width, height, false)) {  // Always software for overlay
        std::cerr << "Overlay compositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
#endif

    // Load settings
    Settings::instance().load();

    // CEF settings
    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
#ifdef __APPLE__
    // macOS: use external_message_pump for responsive input handling
    // (multi_threaded_message_loop only supported on Windows/Linux)
    settings.external_message_pump = true;
#else
    settings.multi_threaded_message_loop = true;
#endif

#ifdef __APPLE__
    // macOS: Must set paths explicitly since we're not a proper .app bundle
    std::filesystem::path framework_path = exe_path / "Frameworks" / "Chromium Embedded Framework.framework";
    CefString(&settings.framework_dir_path).FromString(framework_path.string());
    // Use main executable as subprocess - it handles CefExecuteProcess early
    CefString(&settings.browser_subprocess_path).FromString((exe_path / "jellyfin-desktop").string());
#else
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe").parent_path();
    CefString(&settings.resources_dir_path).FromString(exe_path.string());
    CefString(&settings.locales_dir_path).FromString((exe_path / "locales").string());
#endif

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
    std::cerr << "CEF rendering: " << (use_gpu_overlay ? "GPU (DMA-BUF)" : "software") << std::endl;

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

    // Overlay browser state
    enum class OverlayState { SHOWING, WAITING, FADING, HIDDEN };
    OverlayState overlay_state = OverlayState::SHOWING;
    std::chrono::steady_clock::time_point overlay_fade_start;
    float overlay_browser_alpha = 1.0f;
    std::string pending_server_url;

    // Context menu overlay
    MenuOverlay menu;
    if (!menu.init()) {
        std::cerr << "Warning: Failed to init menu overlay (no font found)" << std::endl;
    }

    // Cursor state
    SDL_Cursor* current_cursor = nullptr;

    // Overlay browser client (for loading UI)
    CefRefPtr<OverlayClient> overlay_client(new OverlayClient(width, height,
        [&](const void* buffer, int w, int h) {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            static bool first_overlay_paint = true;
            if (first_overlay_paint) {
                std::cerr << "[CEF Overlay] first paint callback: " << w << "x" << h << std::endl;
                first_overlay_paint = false;
            }
            void* staging = overlay_compositor.getStagingBuffer(w, h);
            if (staging) {
                memcpy(staging, buffer, w * h * 4);
                overlay_compositor.markStagingDirty();
            } else {
                static bool warned = false;
                if (!warned) {
                    std::cerr << "[CEF Overlay] getStagingBuffer returned null for " << w << "x" << h << std::endl;
                    warned = false;
                }
            }
        },
        [&](const std::string& url) {
            // loadServer callback - start loading main browser
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_server_url = url;
        }
    ));

    CefRefPtr<Client> client(new Client(width, height,
        [&](const void* buffer, int w, int h) {
            // Copy directly to compositor staging buffer (single memcpy)
            std::lock_guard<std::mutex> lock(buffer_mutex);
            static bool first_paint = true;
            if (first_paint) {
                std::cerr << "[CEF] first paint callback: " << w << "x" << h << std::endl;
                first_paint = false;
            }
            void* staging = compositor.getStagingBuffer(w, h);
            if (staging) {
                memcpy(staging, buffer, w * h * 4);
                compositor.markStagingDirty();
            } else {
                static bool warned = false;
                if (!warned) {
                    std::cerr << "[CEF] getStagingBuffer returned null for " << w << "x" << h << std::endl;
                    warned = false;  // Warn every time for debugging
                }
            }
            paint_width = w;
            paint_height = h;
        },
        [&](const std::string& cmd, const std::string& arg, int intArg) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_cmds.push_back({cmd, arg, intArg});
        },
#ifdef __APPLE__
        nullptr,  // No GPU accelerated paint on macOS (using Metal compositor)
#else
        [&](const AcceleratedPaintInfo& info) {
            // GPU accelerated paint - queue DMA-BUF for import in render loop
            compositor.queueDmaBuf(info);
        },
#endif
        &menu,
        [&](cef_cursor_type_t type) {
            SDL_SystemCursor sdl_type = cefCursorToSDL(type);
            if (current_cursor) {
                SDL_DestroyCursor(current_cursor);
            }
            current_cursor = SDL_CreateSystemCursor(sdl_type);
            SDL_SetCursor(current_cursor);
        }
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
        std::cerr << "CEF frame rate: " << mode->refresh_rate << " Hz" << std::endl;
    } else {
        browser_settings.windowless_frame_rate = 60;
    }

    // Create overlay browser loading index.html
    CefWindowInfo overlay_window_info;
    overlay_window_info.SetAsWindowless(0);
    CefBrowserSettings overlay_browser_settings;
    overlay_browser_settings.background_color = 0;
    overlay_browser_settings.windowless_frame_rate = browser_settings.windowless_frame_rate;

    std::string overlay_html_path = "file://" + (exe_path / "resources" / "index.html").string();
    CefBrowserHost::CreateBrowser(overlay_window_info, overlay_client, overlay_html_path, overlay_browser_settings, nullptr, nullptr);

    // State tracking
    using Clock = std::chrono::steady_clock;

    // Main browser: load saved server immediately, or about:blank
    std::string main_url = Settings::instance().serverUrl();
    if (main_url.empty()) {
        main_url = "about:blank";
        std::cerr << "[Overlay] State: SHOWING (no saved URL)" << std::endl;
    } else {
        // Start fade timer since we're auto-loading saved server
        overlay_state = OverlayState::WAITING;
        overlay_fade_start = Clock::now();
        std::cerr << "[Overlay] State: SHOWING -> WAITING (saved URL: " << main_url << ")" << std::endl;
    }
    std::cerr << "Main browser loading: " << main_url << std::endl;
    CefBrowserHost::CreateBrowser(window_info, client, main_url, browser_settings, nullptr, nullptr);
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
    bool video_ready = false;  // Latches true once first frame renders
#ifdef __APPLE__
    auto last_cef_work = Clock::now();
    // Calculate pump interval based on display refresh rate (e.g., 8ms for 120Hz, 16ms for 60Hz)
    int cef_pump_interval_ms = (mode && mode->refresh_rate > 0) ? static_cast<int>(1000.0f / mode->refresh_rate) : 16;
    std::cerr << "CEF pump interval: " << cef_pump_interval_ms << "ms" << std::endl;
#endif

    // Auto-load test video if provided via --video
    if (!test_video.empty()) {
        std::cerr << "[TEST] Loading video: " << test_video << std::endl;
        if (mpv.loadFile(test_video)) {
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
            if (overlay_state == OverlayState::SHOWING || overlay_state == OverlayState::WAITING) {
                overlay_client->sendFocus(true);
                client->sendFocus(false);
            } else {
                client->sendFocus(true);
                overlay_client->sendFocus(false);
            }
            focus_set = true;
        }

        // Event-driven: wait for events when idle, poll when active
        SDL_Event event;
        bool have_event;
#ifdef __APPLE__
        if (needs_render || has_video || compositor.hasPendingContent()) {
#else
        if (needs_render || has_video || compositor.hasPendingContent() || compositor.hasPendingDmaBuf()) {
#endif
            have_event = SDL_PollEvent(&event);
        } else {
            // Short wait - just yield CPU, don't block long (1ms for ~1000Hz max)
            have_event = SDL_WaitEventTimeout(&event, 1);
        }

        while (have_event) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !menu.isOpen()) running = false;
#ifdef __APPLE__
            // Cmd+Q to quit on macOS (no menu bar to provide this)
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_Q &&
                (SDL_GetModState() & SDL_KMOD_GUI)) {
                running = false;
            }
#endif

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
                if (overlay_state == OverlayState::SHOWING || overlay_state == OverlayState::WAITING) {
                    overlay_client->sendMouseMove(mouse_x, mouse_y, mods);
                } else {
                    client->sendMouseMove(mouse_x, mouse_y, mods);
                }
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

                if (overlay_state == OverlayState::SHOWING || overlay_state == OverlayState::WAITING) {
                    overlay_client->sendFocus(true);
                    overlay_client->sendMouseClick(x, y, true, btn, click_count, mods);
                } else {
                    client->sendFocus(true);
                    client->sendMouseClick(x, y, true, btn, click_count, mods);
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                if (overlay_state == OverlayState::SHOWING || overlay_state == OverlayState::WAITING) {
                    overlay_client->sendMouseClick(static_cast<int>(event.button.x), static_cast<int>(event.button.y),
                                           false, event.button.button, click_count, mods);
                } else {
                    client->sendMouseClick(static_cast<int>(event.button.x), static_cast<int>(event.button.y),
                                           false, event.button.button, click_count, mods);
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                if (overlay_state == OverlayState::SHOWING || overlay_state == OverlayState::WAITING) {
                    overlay_client->sendMouseWheel(mouse_x, mouse_y, event.wheel.x, event.wheel.y, mods);
                } else {
                    client->sendMouseWheel(mouse_x, mouse_y, event.wheel.x, event.wheel.y, mods);
                }
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                if (overlay_state == OverlayState::SHOWING || overlay_state == OverlayState::WAITING) {
                    overlay_client->sendFocus(true);
                } else {
                    client->sendFocus(true);
                }
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                if (overlay_state == OverlayState::SHOWING || overlay_state == OverlayState::WAITING) {
                    overlay_client->sendFocus(false);
                } else {
                    client->sendFocus(false);
                }
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                auto resize_start = std::chrono::steady_clock::now();
                current_width = event.window.data1;
                current_height = event.window.data2;

#ifdef __APPLE__
                // Resize Metal compositor and video layer
                compositor.resize(current_width, current_height);
                overlay_compositor.resize(current_width, current_height);
                client->resize(current_width, current_height);
                overlay_client->resize(current_width, current_height);

                if (has_subsurface) {
                    videoLayer.resize(current_width, current_height);
                }
#else
                // Resize EGL context (handles wl_egl_window resize)
                egl.resize(current_width, current_height);
                compositor.resize(current_width, current_height);
                overlay_compositor.resize(current_width, current_height);
                client->resize(current_width, current_height);
                overlay_client->resize(current_width, current_height);

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
                std::cerr << "[SDL] KEY " << (down ? "DOWN" : "UP") << ": key=0x" << std::hex << key << std::dec << " mods=" << mods << std::endl;
                bool is_control_key = (key == SDLK_BACKSPACE || key == SDLK_DELETE ||
                    key == SDLK_RETURN || key == SDLK_ESCAPE || key == SDLK_SPACE ||
                    key == SDLK_TAB || key == SDLK_LEFT || key == SDLK_RIGHT ||
                    key == SDLK_UP || key == SDLK_DOWN || key == SDLK_HOME ||
                    key == SDLK_END || key == SDLK_PAGEUP || key == SDLK_PAGEDOWN ||
                    key == SDLK_F5 || key == SDLK_F11);
                bool has_ctrl = (mods & (1 << 2)) != 0;  // CTRL
#ifdef __APPLE__
                bool has_cmd = (mods & (1 << 7)) != 0;   // CMD (macOS)
#else
                bool has_cmd = false;
#endif
                if (is_control_key || has_ctrl || has_cmd) {
                    if (overlay_state == OverlayState::SHOWING || overlay_state == OverlayState::WAITING) {
                        overlay_client->sendKeyEvent(key, down, mods);
                    } else {
                        client->sendKeyEvent(key, down, mods);
                    }
                }
            } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                std::cerr << "[SDL] TEXT_INPUT: \"" << event.text.text << "\"" << std::endl;
                for (const char* c = event.text.text; *c; ++c) {
                    if (overlay_state == OverlayState::SHOWING || overlay_state == OverlayState::WAITING) {
                        overlay_client->sendChar(static_cast<unsigned char>(*c), mods);
                    } else {
                        client->sendChar(static_cast<unsigned char>(*c), mods);
                    }
                }
            }
            have_event = SDL_PollEvent(&event);
        }

#ifdef __APPLE__
        // macOS: external_message_pump - call CefDoMessageLoopWork when CEF requests it
        if (App::NeedsWork()) {
            int64_t delay_ms = App::GetWorkDelay();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_cef_work).count();
            if (delay_ms == 0 || elapsed >= delay_ms) {
                CefDoMessageLoopWork();
                last_cef_work = Clock::now();
            }
        }
        // Also pump periodically to ensure responsiveness (matches display refresh rate)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last_cef_work).count();
            if (elapsed >= cef_pump_interval_ms) {
                CefDoMessageLoopWork();
                last_cef_work = Clock::now();
            }
        }
#endif

        // Determine if we need to render this frame
        // With vsync, always render to maintain consistent frame pacing
#ifdef __APPLE__
        needs_render = activity_this_frame || has_video || compositor.hasPendingContent() || overlay_state == OverlayState::FADING;
#else
        needs_render = activity_this_frame || has_video || compositor.hasPendingContent() || compositor.hasPendingDmaBuf() || overlay_state == OverlayState::FADING;
#endif

        if (activity_this_frame) {
            last_activity = now;
        }

        // Process player commands
        {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            for (const auto& cmd : pending_cmds) {
                if (cmd.cmd == "load") {
                    double startSec = static_cast<double>(cmd.intArg) / 1000.0;
                    std::cerr << "[MAIN] playerLoad: " << cmd.url << " start=" << startSec << "s" << std::endl;
                    if (mpv.loadFile(cmd.url, startSec)) {
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
                    video_ready = false;
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

        // Check for pending server URL from overlay
        {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            if (!pending_server_url.empty()) {
                std::string url = pending_server_url;
                pending_server_url.clear();

                // Only process if we're still showing the overlay form
                // (ignore if already loading/fading from saved server)
                if (overlay_state == OverlayState::SHOWING) {
                    // Save URL to settings
                    Settings::instance().setServerUrl(url);
                    Settings::instance().save();

                    // Load in main browser
                    client->loadUrl(url);

                    // Start fade timer
                    overlay_state = OverlayState::WAITING;
                    overlay_fade_start = now;
                }
            }
        }

        // Update overlay state machine
        if (overlay_state == OverlayState::WAITING) {
            auto elapsed = std::chrono::duration<float>(now - overlay_fade_start).count();
            if (elapsed >= OVERLAY_FADE_DELAY_SEC) {
                overlay_state = OverlayState::FADING;
                overlay_fade_start = now;
                std::cerr << "[Overlay] State: WAITING -> FADING" << std::endl;
            }
        } else if (overlay_state == OverlayState::FADING) {
            auto elapsed = std::chrono::duration<float>(now - overlay_fade_start).count();
            float progress = elapsed / OVERLAY_FADE_DURATION_SEC;
            if (progress >= 1.0f) {
                overlay_browser_alpha = 0.0f;
                overlay_state = OverlayState::HIDDEN;
                // Hide overlay view so old content doesn't show through
                overlay_compositor.setVisible(false);
                // Transfer focus from overlay to main browser
                overlay_client->sendFocus(false);
                client->sendFocus(true);
                std::cerr << "[Overlay] State: FADING -> HIDDEN" << std::endl;
            } else {
                overlay_browser_alpha = 1.0f - progress;
            }
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
                video_ready = true;
            }
        }

        // Composite main browser (Metal handles its own presentation)
        // Always call composite() - it handles "no content yet" internally and uploads staging data
        if (test_video.empty() && (compositor.hasValidOverlay() || compositor.hasPendingContent())) {
            float alpha = video_ready ? overlay_alpha : 1.0f;
            compositor.composite(current_width, current_height, alpha);
        }

        // Composite overlay browser (with fade alpha)
        if (overlay_state != OverlayState::HIDDEN && overlay_browser_alpha > 0.01f) {
            if (overlay_compositor.hasValidOverlay() || overlay_compositor.hasPendingContent()) {
                overlay_compositor.composite(current_width, current_height, overlay_browser_alpha);
            }
        }
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
                video_ready = true;
            }
        }

        // Import queued DMA-BUF if any (GPU path)
        compositor.importQueuedDmaBuf();

        // Flush pending overlay data to GPU texture (software path)
        compositor.flushOverlay();

        // Clear main surface (transparent when video ready, black otherwise)
        float bg_alpha = video_ready ? 0.0f : 1.0f;
        glClearColor(0.0f, 0.0f, 0.0f, bg_alpha);
        glClear(GL_COLOR_BUFFER_BIT);

        // Composite main browser (always full opacity when no video)
        if (test_video.empty() && compositor.hasValidOverlay()) {
            float alpha = video_ready ? overlay_alpha : 1.0f;
            compositor.composite(current_width, current_height, alpha);
        }

        // Composite overlay browser (with fade alpha)
        if (overlay_state != OverlayState::HIDDEN && overlay_browser_alpha > 0.01f) {
            overlay_compositor.flushOverlay();
            if (overlay_compositor.hasValidOverlay()) {
                overlay_compositor.composite(current_width, current_height, overlay_browser_alpha);
            }
        }

        // Swap buffers
        egl.swapBuffers();
#endif
    }

    // Cleanup
    mpv.cleanup();
    compositor.cleanup();
    overlay_compositor.cleanup();
#ifdef __APPLE__
    if (has_subsurface) {
        videoLayer.cleanup();
    }
#else
    if (has_subsurface) {
        subsurface.cleanup();
    }
    egl.cleanup();
#endif

    CefShutdown();
    if (current_cursor) {
        SDL_DestroyCursor(current_cursor);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
