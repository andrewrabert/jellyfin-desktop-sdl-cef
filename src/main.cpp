#include <SDL3/SDL.h>
#include <iostream>
#include <filesystem>
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
void activateMacWindow(SDL_Window* window);
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include "platform/macos_layer.h"
#include "compositor/metal_compositor.h"
#include "player/media_session.h"
#include "player/macos/media_session_macos.h"
#else
#include "context/egl_context.h"
#include "platform/wayland_subsurface.h"
#include "player/media_session.h"
#include "player/mpris/media_session_mpris.h"
#include "compositor/opengl_compositor.h"
#endif
#include "player/mpv/mpv_player_vk.h"
#include "cef/cef_app.h"
#include "cef/cef_client.h"
#include "input/input_layer.h"
#include "input/browser_layer.h"
#include "input/menu_layer.h"
#include "input/mpv_layer.h"
#include "input/window_state.h"
#include "ui/menu_overlay.h"
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

// Simple JSON string value extractor (handles escaped quotes)
std::string jsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;  // Skip opening quote
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;  // Skip escape char
        }
        result += json[pos++];
    }
    return result;
}

// Extract integer from JSON
int64_t jsonGetInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string num;
    while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '-')) {
        num += json[pos++];
    }
    return num.empty() ? 0 : std::stoll(num);
}

// Extract double from JSON (with optional hasValue output)
double jsonGetDouble(const std::string& json, const std::string& key, bool* hasValue = nullptr) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        if (hasValue) *hasValue = false;
        return 0.0;
    }
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    std::string num;
    while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '-' || json[pos] == '.' || json[pos] == 'e' || json[pos] == 'E' || json[pos] == '+')) {
        num += json[pos++];
    }
    if (hasValue) *hasValue = !num.empty();
    return num.empty() ? 0.0 : std::stod(num);
}

// Extract first element from JSON array of strings
std::string jsonGetFirstArrayString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    while (pos < json.size() && json[pos] != '[') pos++;
    if (pos >= json.size()) return "";
    pos++;  // Skip [
    while (pos < json.size() && json[pos] != '"' && json[pos] != ']') pos++;
    if (pos >= json.size() || json[pos] == ']') return "";
    pos++;  // Skip opening quote
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) pos++;
        result += json[pos++];
    }
    return result;
}

MediaMetadata parseMetadataJson(const std::string& json) {
    MediaMetadata meta;
    meta.title = jsonGetString(json, "Name");
    // For episodes, use SeriesName as artist; for audio, use Artists array
    meta.artist = jsonGetString(json, "SeriesName");
    if (meta.artist.empty()) {
        meta.artist = jsonGetFirstArrayString(json, "Artists");
    }
    // For episodes, use SeasonName as album; for audio, use Album
    meta.album = jsonGetString(json, "SeasonName");
    if (meta.album.empty()) {
        meta.album = jsonGetString(json, "Album");
    }
    meta.track_number = static_cast<int>(jsonGetInt(json, "IndexNumber"));
    // RunTimeTicks is in 100ns units, convert to microseconds
    meta.duration_us = jsonGetInt(json, "RunTimeTicks") / 10;
    // Detect media type from Type field
    std::string type = jsonGetString(json, "Type");
    if (type == "Audio") {
        meta.media_type = MediaType::Audio;
    } else if (type == "Movie" || type == "Episode" || type == "Video" || type == "MusicVideo") {
        meta.media_type = MediaType::Video;
    }
    return meta;
}

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
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: jellyfin-desktop [options]\n"
                      << "\nOptions:\n"
                      << "  -h, --help       Show this help message\n"
                      << "  --video <file>   Load video file on startup\n"
                      << "  --gpu-overlay    Enable GPU overlay (experimental)\n";
            return 0;
        } else if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) {
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
        "Jellyfin Desktop CEF",
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
    // Window activation is deferred until first WINDOW_EXPOSED event
    // to ensure the window is actually visible before activating
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
    double current_playback_rate = 1.0;
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
    bool video_needs_rerender = false;  // Force render after resize (for paused video)
    double current_playback_rate = 1.0;
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
        double doubleArg;
        std::string metadata;  // JSON for load command
    };
    std::mutex cmd_mutex;
    std::vector<PlayerCmd> pending_cmds;

    // Initialize media session with platform backend
    MediaSession mediaSession;
#ifdef __APPLE__
    mediaSession.addBackend(createMacOSMediaBackend(&mediaSession));
#else
    mediaSession.addBackend(createMprisBackend(&mediaSession));
#endif
    mediaSession.onPlay = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "play", 0, 0.0});
    };
    mediaSession.onPause = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "pause", 0, 0.0});
    };
    mediaSession.onPlayPause = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "play_pause", 0, 0.0});
    };
    mediaSession.onStop = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "stop", 0, 0.0});
    };
    mediaSession.onSeek = [&](int64_t position_us) {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_seek", "", static_cast<int>(position_us / 1000), 0.0});
    };
    mediaSession.onNext = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "next", 0, 0.0});
    };
    mediaSession.onPrevious = [&]() {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_action", "previous", 0, 0.0});
    };
    mediaSession.onRaise = [&]() {
        SDL_RaiseWindow(window);
    };
    mediaSession.onSetRate = [&](double rate) {
        std::lock_guard<std::mutex> lock(cmd_mutex);
        pending_cmds.push_back({"media_rate", "", 0, rate});
    };

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
                    warned = true;
                }
            }
        },
        [&](const std::string& url) {
            // loadServer callback - start loading main browser
            std::cerr << "[Overlay] loadServer callback: " << url << std::endl;
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_server_url = url;
        }
    ));

    // Track who initiated fullscreen (only changes from NONE, returns to NONE on exit)
    enum class FullscreenSource { NONE, WM, CEF };
    FullscreenSource fullscreen_source = FullscreenSource::NONE;

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
                    warned = true;
                }
            }
            paint_width = w;
            paint_height = h;
        },
        [&](const std::string& cmd, const std::string& arg, int intArg, const std::string& metadata) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_cmds.push_back({cmd, arg, intArg, 0.0, metadata});
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
        },
        [&](bool fullscreen) {
            // Web content requested fullscreen change via JS Fullscreen API
            std::cerr << "[Fullscreen] CEF requests " << (fullscreen ? "enter" : "exit")
                      << ", source=" << static_cast<int>(fullscreen_source) << std::endl;
            if (fullscreen) {
                if (fullscreen_source == FullscreenSource::NONE) {
                    fullscreen_source = FullscreenSource::CEF;
                }
                SDL_SetWindowFullscreen(window, true);
            } else {
                // Only honor CEF exit if CEF initiated fullscreen
                if (fullscreen_source == FullscreenSource::CEF) {
                    SDL_SetWindowFullscreen(window, false);
                    fullscreen_source = FullscreenSource::NONE;
                }
                // WM-initiated fullscreen: ignore CEF exit request
            }
        }
    ));

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
#if defined(CEF_X11) || defined(__linux__)
    window_info.shared_texture_enabled = use_gpu_overlay;
#endif

    CefBrowserSettings browser_settings;
    browser_settings.background_color = 0;
    browser_settings.javascript_access_clipboard = STATE_ENABLED;
    browser_settings.javascript_dom_paste = STATE_ENABLED;
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

    std::string overlay_html_path = "app://resources/index.html";
    CefBrowserHost::CreateBrowser(overlay_window_info, overlay_client, overlay_html_path, overlay_browser_settings, nullptr, nullptr);

    // State tracking
    using Clock = std::chrono::steady_clock;

    // Main browser: load saved server immediately, or wait for overlay IPC
    std::string saved_url = Settings::instance().serverUrl();
    if (saved_url.empty()) {
        // No saved server - create with blank, wait for overlay loadServer IPC
        std::cerr << "[Main] Waiting for overlay to provide server URL" << std::endl;
        CefBrowserHost::CreateBrowser(window_info, client, "about:blank", browser_settings, nullptr, nullptr);
    } else {
        // Have saved server - start loading immediately, begin overlay fade
        overlay_state = OverlayState::WAITING;
        overlay_fade_start = Clock::now();
        std::cerr << "[Main] Loading saved server: " << saved_url << std::endl;
        CefBrowserHost::CreateBrowser(window_info, client, saved_url, browser_settings, nullptr, nullptr);
    }
    // Input routing stack
    BrowserLayer overlay_browser_layer(overlay_client.get());
    BrowserLayer main_browser_layer(client.get());
    overlay_browser_layer.setWindowSize(width, height);
    main_browser_layer.setWindowSize(width, height);
    MenuLayer menu_layer(&menu);
    InputStack input_stack;
    input_stack.push(&overlay_browser_layer);  // Start with overlay

    // Track which browser layer is active
    BrowserLayer* active_browser = &overlay_browser_layer;

    // Push/pop menu layer on open/close
    menu.setOnOpen([&]() { input_stack.push(&menu_layer); });
    menu.setOnClose([&]() { input_stack.remove(&menu_layer); });

    // Window state notifications
    WindowStateNotifier window_state;
    window_state.add(active_browser);
    MpvLayer mpv_layer(&mpv);
    window_state.add(&mpv_layer);

    auto last_activity = Clock::now();
    float overlay_alpha = 1.0f;
    bool focus_set = false;
    int current_width = width;
    int current_height = height;
    bool video_ready = false;  // Latches true once first frame renders
#ifdef __APPLE__
    bool window_activated = false;  // Activate window on first expose event
    auto last_cef_work = Clock::now();
    // Calculate pump interval based on display refresh rate (e.g., 8ms for 120Hz, 16ms for 60Hz)
    int cef_pump_interval_ms = (mode && mode->refresh_rate > 0) ? static_cast<int>(1000.0f / mode->refresh_rate) : 16;
    std::cerr << "CEF pump interval: " << cef_pump_interval_ms << "ms" << std::endl;
#endif

    // Set up mpv event callbacks (event-driven like jellyfin-desktop)
    mpv.setPositionCallback([&](double ms) {
        mediaSession.setPosition(static_cast<int64_t>(ms * 1000.0));
    });
    mpv.setDurationCallback([&](double ms) {
        client->updateDuration(ms);
    });
    mpv.setPlayingCallback([&]() {
        // FILE_LOADED - initial playback start
        client->emitPlaying();
        client->updatePosition(mpv.getPosition());
        mediaSession.setPosition(static_cast<int64_t>(mpv.getPosition() * 1000.0));
        mediaSession.setPlaybackState(PlaybackState::Playing);
    });
    mpv.setStateCallback([&](bool paused) {
        // Ignore initial property emission before any file is loaded
        if (!mpv.isPlaying()) return;

        // pause property changed - pause/resume
        double pos = mpv.getPosition();
        client->updatePosition(pos);
        if (paused) {
            client->emitPaused();
            mediaSession.setPosition(static_cast<int64_t>(pos * 1000.0));
            mediaSession.setPlaybackState(PlaybackState::Paused);
        } else {
            client->emitPlaying();
            mediaSession.setPosition(static_cast<int64_t>(pos * 1000.0));
            mediaSession.setPlaybackState(PlaybackState::Playing);
        }
    });
    mpv.setFinishedCallback([&]() {
        std::cerr << "[MAIN] Track finished naturally (EOF), emitting finished signal" << std::endl;
        has_video = false;
#ifndef __APPLE__
        if (has_subsurface) {
            subsurface.setVisible(false);
        }
#endif
        client->emitFinished();
        mediaSession.setPlaybackState(PlaybackState::Stopped);
    });
    mpv.setCanceledCallback([&]() {
        std::cerr << "[MAIN] Track canceled (user stop), emitting canceled signal" << std::endl;
        has_video = false;
#ifndef __APPLE__
        if (has_subsurface) {
            subsurface.setVisible(false);
        }
#endif
        client->emitCanceled();
        mediaSession.setPlaybackState(PlaybackState::Stopped);
    });
    mpv.setSeekedCallback([&](double ms) {
        client->updatePosition(ms);
        mediaSession.setPosition(static_cast<int64_t>(ms * 1000.0));
        mediaSession.setRate(current_playback_rate);
        mediaSession.emitSeeked(static_cast<int64_t>(ms * 1000.0));
    });
    mpv.setBufferingCallback([&](bool buffering, double ms) {
        mediaSession.setPosition(static_cast<int64_t>(ms * 1000.0));
        if (buffering) {
            mediaSession.setRate(0.0);
        } else {
            mediaSession.setRate(current_playback_rate);
        }
    });
    mpv.setBufferedRangesCallback([&](const std::vector<MpvPlayerVk::BufferedRange>& ranges) {
        // Send buffered ranges to JS as JSON array
        std::string json = "[";
        for (size_t i = 0; i < ranges.size(); i++) {
            if (i > 0) json += ",";
            json += "{\"start\":" + std::to_string(ranges[i].start) +
                    ",\"end\":" + std::to_string(ranges[i].end) + "}";
        }
        json += "]";
        client->executeJS("if(window._nativeUpdateBufferedRanges)window._nativeUpdateBufferedRanges(" + json + ");");
    });
    mpv.setCoreIdleCallback([&](bool idle, double ms) {
        (void)idle;
        mediaSession.setPosition(static_cast<int64_t>(ms * 1000.0));
    });

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

        // Process mpv events (event-driven position/state updates)
        mpv.processEvents();

        if (!focus_set) {
            window_state.notifyFocusGained();
            focus_set = true;
        }

        // Process media session events
        mediaSession.update();

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
                event.type == SDL_EVENT_KEY_UP ||
                event.type == SDL_EVENT_FINGER_DOWN ||
                event.type == SDL_EVENT_FINGER_UP ||
                event.type == SDL_EVENT_FINGER_MOTION) {
                activity_this_frame = true;
            }

            // Route input through layer stack
            if (event.type == SDL_EVENT_MOUSE_MOTION ||
                event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                event.type == SDL_EVENT_MOUSE_WHEEL ||
                event.type == SDL_EVENT_KEY_DOWN ||
                event.type == SDL_EVENT_KEY_UP ||
                event.type == SDL_EVENT_TEXT_INPUT ||
                event.type == SDL_EVENT_FINGER_DOWN ||
                event.type == SDL_EVENT_FINGER_UP ||
                event.type == SDL_EVENT_FINGER_MOTION) {
                input_stack.route(event);
            }

            // Window events handled separately
            if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                window_state.notifyFocusGained();
                // Sync browser fullscreen with SDL state on focus gain (WM may have changed it)
                bool isFs = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                if (isFs) {
                    client->executeJS("document.documentElement.requestFullscreen().catch(()=>{});");
                } else {
                    client->exitFullscreen();
                }
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                window_state.notifyFocusLost();
            } else if (event.type == SDL_EVENT_WINDOW_MINIMIZED) {
                window_state.notifyMinimized();
            } else if (event.type == SDL_EVENT_WINDOW_RESTORED) {
                window_state.notifyRestored();
#ifdef __APPLE__
            } else if (event.type == SDL_EVENT_WINDOW_EXPOSED && !window_activated) {
                // Activate window once it's actually visible on screen
                activateMacWindow(window);
                window_activated = true;
#endif
            } else if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN) {
                // WM initiated fullscreen - track source and sync browser state
                std::cerr << "[Fullscreen] SDL enter, source=" << static_cast<int>(fullscreen_source) << std::endl;
                if (fullscreen_source == FullscreenSource::NONE) {
                    fullscreen_source = FullscreenSource::WM;
                }
                client->executeJS("document.documentElement.requestFullscreen().catch(()=>{});");
            } else if (event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
                // WM exited fullscreen - always sync browser, only clear source if WM initiated
                std::cerr << "[Fullscreen] SDL leave, source=" << static_cast<int>(fullscreen_source) << std::endl;
                client->exitFullscreen();
                if (fullscreen_source == FullscreenSource::WM) {
                    fullscreen_source = FullscreenSource::NONE;
                }
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                auto resize_start = std::chrono::steady_clock::now();
                current_width = event.window.data1;
                current_height = event.window.data2;
                overlay_browser_layer.setWindowSize(current_width, current_height);
                main_browser_layer.setWindowSize(current_width, current_height);

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
                    video_needs_rerender = true;  // Force render even when paused
                }
#endif

                auto resize_end = std::chrono::steady_clock::now();
                std::cerr << "[" << _ms() << "ms] resize: total="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(resize_end-resize_start).count()
                          << "ms" << std::endl;
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
                    // Parse and set media session metadata
                    if (!cmd.metadata.empty() && cmd.metadata != "{}") {
                        MediaMetadata meta = parseMetadataJson(cmd.metadata);
                        std::cerr << "[MAIN] metadata: title=" << meta.title << " artist=" << meta.artist << std::endl;
                        mediaSession.setMetadata(meta);
                        // Apply normalization gain (ReplayGain) if present
                        bool hasGain = false;
                        double normGain = jsonGetDouble(cmd.metadata, "NormalizationGain", &hasGain);
                        mpv.setNormalizationGain(hasGain ? normGain : 0.0);
                    } else {
                        mpv.setNormalizationGain(0.0);  // Clear any previous gain
                    }
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
                        // mpv events will trigger state callbacks
                    } else {
                        client->emitError("Failed to load video");
                    }
                } else if (cmd.cmd == "stop") {
                    mpv.stop();
                    has_video = false;
                    video_ready = false;
#ifndef __APPLE__
                    if (has_subsurface) {
                        subsurface.setVisible(false);
                    }
#endif
                    // mpv END_FILE event will trigger finished callback
                } else if (cmd.cmd == "pause") {
                    mpv.pause();
                    // mpv pause property change will trigger state callback
                } else if (cmd.cmd == "play") {
                    mpv.play();
                    // mpv pause property change will trigger state callback
                } else if (cmd.cmd == "playpause") {
                    if (mpv.isPaused()) {
                        mpv.play();
                    } else {
                        mpv.pause();
                    }
                    // mpv pause property change will trigger state callback
                } else if (cmd.cmd == "seek") {
                    mpv.seek(static_cast<double>(cmd.intArg) / 1000.0);
                } else if (cmd.cmd == "volume") {
                    mpv.setVolume(cmd.intArg);
                } else if (cmd.cmd == "mute") {
                    mpv.setMuted(cmd.intArg != 0);
                } else if (cmd.cmd == "speed") {
                    double speed = cmd.intArg / 1000.0;
                    mpv.setSpeed(speed);
                } else if (cmd.cmd == "media_metadata") {
                    MediaMetadata meta = parseMetadataJson(cmd.url);
                    std::cerr << "[MAIN] Media metadata: title=" << meta.title << std::endl;
                    mediaSession.setMetadata(meta);
                } else if (cmd.cmd == "media_position") {
                    int64_t pos_us = static_cast<int64_t>(cmd.intArg) * 1000;
                    mediaSession.setPosition(pos_us);
                } else if (cmd.cmd == "media_state") {
                    if (cmd.url == "Playing") {
                        mediaSession.setPlaybackState(PlaybackState::Playing);
                    } else if (cmd.url == "Paused") {
                        mediaSession.setPlaybackState(PlaybackState::Paused);
                    } else {
                        mediaSession.setPlaybackState(PlaybackState::Stopped);
                    }
                } else if (cmd.cmd == "media_artwork") {
                    std::cerr << "[MAIN] Media artwork received: " << cmd.url.substr(0, 50) << "..." << std::endl;
                    mediaSession.setArtwork(cmd.url);
                } else if (cmd.cmd == "media_queue") {
                    // Decode flags: bit 0 = canNext, bit 1 = canPrev
                    bool canNext = (cmd.intArg & 1) != 0;
                    bool canPrev = (cmd.intArg & 2) != 0;
                    mediaSession.setCanGoNext(canNext);
                    mediaSession.setCanGoPrevious(canPrev);
                } else if (cmd.cmd == "media_notify_rate") {
                    // Rate was encoded as rate * 1000000
                    double rate = static_cast<double>(cmd.intArg) / 1000000.0;
                    current_playback_rate = rate;
                    mediaSession.setRate(rate);
                } else if (cmd.cmd == "media_seeked") {
                    // JS detected a seek - emit Seeked signal to media session
                    int64_t pos_us = static_cast<int64_t>(cmd.intArg) * 1000;
                    mediaSession.emitSeeked(pos_us);
                } else if (cmd.cmd == "media_action") {
                    // Route media session control commands to JS playbackManager
                    std::string js = "if(window._nativeHostInput) window._nativeHostInput(['" + cmd.url + "']);";
                    client->executeJS(js);
                } else if (cmd.cmd == "media_seek") {
                    // Route media session seek to JS playbackManager
                    std::string js = "if(window._nativeSeek) window._nativeSeek(" + std::to_string(cmd.intArg) + ");";
                    client->executeJS(js);
                } else if (cmd.cmd == "media_rate") {
                    // Route media session rate change to JS player
                    client->emitRateChanged(cmd.doubleArg);
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
                    std::cerr << "[Main] Loading server from overlay: " << url << std::endl;
                    Settings::instance().setServerUrl(url);
                    Settings::instance().save();
                    client->loadUrl(url);
                    overlay_state = OverlayState::WAITING;
                    overlay_fade_start = now;
                } else {
                    std::cerr << "[Main] Ignoring loadServer (overlay_state != SHOWING)" << std::endl;
                }
            }
        }

        // Update overlay state machine
        if (overlay_state == OverlayState::WAITING) {
            auto elapsed = std::chrono::duration<float>(now - overlay_fade_start).count();
            if (elapsed >= OVERLAY_FADE_DELAY_SEC) {
                overlay_state = OverlayState::FADING;
                // Switch input from overlay to main browser
                window_state.remove(active_browser);
                active_browser->onFocusLost();
                input_stack.remove(&overlay_browser_layer);
                input_stack.push(&main_browser_layer);
                active_browser = &main_browser_layer;
                window_state.add(active_browser);
                active_browser->onFocusGained();
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
        if (has_subsurface && ((has_video && mpv.hasFrame()) || video_needs_rerender)) {
            VkImage sub_image;
            VkImageView sub_view;
            VkFormat sub_format;
            if (subsurface.startFrame(&sub_image, &sub_view, &sub_format)) {
                mpv.render(sub_image, sub_view,
                          subsurface.width(), subsurface.height(),
                          sub_format);
                subsurface.submitFrame();
                video_ready = true;
                video_needs_rerender = false;
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
