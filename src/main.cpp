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
#include <array>
#include <atomic>

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
#include "PFMoveApplication.h"
#include "player/mpv/mpv_player_vk.h"
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "context/wgl_context.h"
#include "player/media_session.h"
#include "compositor/opengl_compositor.h"
#include "player/mpv/mpv_player_gl.h"
#else
#include "context/egl_context.h"
#include "platform/wayland_subsurface.h"
#include "player/media_session.h"
#include "player/mpris/media_session_mpris.h"
#include "compositor/opengl_compositor.h"
#include "player/mpv/mpv_player_vk.h"
#include "player/mpv/mpv_player_gl.h"
#endif
#include "cef/cef_app.h"
#include "cef/cef_client.h"
#include "input/input_layer.h"
#include "input/browser_layer.h"
#include "input/menu_layer.h"
#include "input/mpv_layer.h"
#include "input/window_state.h"
#include "ui/menu_overlay.h"
#include "settings.h"

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

// Extract integer from JSON with default value
int jsonGetIntDefault(const std::string& json, const std::string& key, int defaultVal) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return defaultVal;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return defaultVal;
    bool negative = false;
    if (json[pos] == '-') { negative = true; pos++; }
    int val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return negative ? -val : val;
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
    // Check if running from app bundle (exe is in Contents/MacOS/) or dev build
    std::filesystem::path cef_framework_path;
    if (exe_path.parent_path().filename() == "Contents") {
        // App bundle: framework is at ../Frameworks/
        cef_framework_path = exe_path.parent_path() / "Frameworks";
    } else {
        // Dev build: framework is at ./Frameworks/
        cef_framework_path = exe_path / "Frameworks";
    }
    std::string framework_lib = (cef_framework_path /
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
#ifdef _WIN32
    CefMainArgs main_args(GetModuleHandle(NULL));
#else
    CefMainArgs main_args(argc, argv);
#endif
    CefRefPtr<App> app(new App());

    std::cerr << "Calling CefExecuteProcess..." << std::endl;
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    std::cerr << "CefExecuteProcess returned: " << exit_code << std::endl;
    if (exit_code >= 0) {
        return exit_code;
    }

#if defined(__APPLE__) && defined(NDEBUG)
    // In release builds, offer to move app to /Applications (clears quarantine)
    PFMoveToApplicationsFolderIfNecessary();
#endif

    // SDL initialization with OpenGL (for main surface CEF overlay)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    const int width = 1280;
    const int height = 720;

    // Use plain Wayland window - we create our own EGL context
    // SDL_WINDOW_HIGH_PIXEL_DENSITY enables HiDPI support
    SDL_Window* window = SDL_CreateWindow(
        "Jellyfin Desktop CEF",
        width, height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
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

    // Player dispatch helpers (macOS has single Vulkan player)
    auto mpvLoadFile = [&](const std::string& path, double startSec = 0.0) {
        return mpv.loadFile(path, startSec);
    };
    auto mpvStop = [&]() { mpv.stop(); };
    auto mpvPause = [&]() { mpv.pause(); };
    auto mpvPlay = [&]() { mpv.play(); };
    auto mpvSeek = [&](double sec) { mpv.seek(sec); };
    auto mpvSetVolume = [&](int vol) { mpv.setVolume(vol); };
    auto mpvSetMuted = [&](bool m) { mpv.setMuted(m); };
    auto mpvSetSpeed = [&](double s) { mpv.setSpeed(s); };
    auto mpvSetNormalizationGain = [&](double g) { mpv.setNormalizationGain(g); };
    auto mpvSetSubtitleTrack = [&](int sid) { mpv.setSubtitleTrack(sid); };
    auto mpvSetAudioTrack = [&](int aid) { mpv.setAudioTrack(aid); };
    auto mpvSetAudioDelay = [&](double d) { mpv.setAudioDelay(d); };
    auto mpvIsPaused = [&]() { return mpv.isPaused(); };
    auto mpvIsPlaying = [&]() { return mpv.isPlaying(); };
    auto mpvHasFrame = [&]() { return mpv.hasFrame(); };
    auto mpvProcessEvents = [&]() { mpv.processEvents(); };
    auto mpvCleanup = [&]() { mpv.cleanup(); };

    // Initialize Metal compositor for CEF overlay (renders on top of video)
    float initial_scale = SDL_GetWindowDisplayScale(window);
    int physical_width = static_cast<int>(width * initial_scale);
    int physical_height = static_cast<int>(height * initial_scale);
    std::cerr << "[macOS HiDPI] scale=" << initial_scale
              << " logical=" << width << "x" << height
              << " physical=" << physical_width << "x" << physical_height << std::endl;

    MetalCompositor compositor;
    if (!compositor.init(window, physical_width, physical_height)) {
        std::cerr << "MetalCompositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Second compositor for overlay browser
    MetalCompositor overlay_compositor;
    if (!overlay_compositor.init(window, physical_width, physical_height)) {
        std::cerr << "Overlay compositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
#elif defined(_WIN32)
    // Windows: Initialize WGL context for OpenGL rendering
    WGLContext wgl;
    if (!wgl.init(window)) {
        std::cerr << "WGL init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize mpv player with OpenGL backend (better Wine compatibility)
    MpvPlayerGL mpv;
    bool has_video = false;
    bool video_needs_rerender = false;
    double current_playback_rate = 1.0;
    bool has_subsurface = false;  // No separate video layer on Windows OpenGL path
    if (!mpv.init(&wgl)) {
        std::cerr << "MpvPlayerGL init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    std::cerr << "Using OpenGL for video rendering (no HDR)" << std::endl;

    // Player dispatch helpers (Windows has single OpenGL player)
    auto mpvLoadFile = [&](const std::string& path, double startSec = 0.0) {
        return mpv.loadFile(path, startSec);
    };
    auto mpvStop = [&]() { mpv.stop(); };
    auto mpvPause = [&]() { mpv.pause(); };
    auto mpvPlay = [&]() { mpv.play(); };
    auto mpvSeek = [&](double sec) { mpv.seek(sec); };
    auto mpvSetVolume = [&](int vol) { mpv.setVolume(vol); };
    auto mpvSetMuted = [&](bool m) { mpv.setMuted(m); };
    auto mpvSetSpeed = [&](double s) { mpv.setSpeed(s); };
    auto mpvSetNormalizationGain = [&](double g) { mpv.setNormalizationGain(g); };
    auto mpvSetSubtitleTrack = [&](int sid) { mpv.setSubtitleTrack(sid); };
    auto mpvSetAudioTrack = [&](int aid) { mpv.setAudioTrack(aid); };
    auto mpvSetAudioDelay = [&](double d) { mpv.setAudioDelay(d); };
    auto mpvIsPaused = [&]() { return mpv.isPaused(); };
    auto mpvIsPlaying = [&]() { return mpv.isPlaying(); };
    auto mpvHasFrame = [&]() { return mpv.hasFrame(); };
    auto mpvProcessEvents = [&]() { mpv.processEvents(); };
    auto mpvCleanup = [&]() { mpv.cleanup(); };

    // Initialize OpenGL compositor for CEF overlay
    OpenGLCompositor compositor;
    if (!compositor.init(&wgl, width, height, false)) {  // Software path only on Windows for now
        std::cerr << "OpenGLCompositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Second compositor for overlay browser
    OpenGLCompositor overlay_compositor;
    if (!overlay_compositor.init(&wgl, width, height, false)) {
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

    // Detect Wayland vs X11 at runtime
    const char* videoDriver = SDL_GetCurrentVideoDriver();
    bool useWayland = videoDriver && strcmp(videoDriver, "wayland") == 0;
    std::cerr << "SDL video driver: " << (videoDriver ? videoDriver : "null")
              << " -> using " << (useWayland ? "Wayland" : "X11") << std::endl;

    // Video layer (Wayland subsurface only - X11 uses OpenGL composition)
    WaylandSubsurface waylandSubsurface;
    bool has_subsurface = false;
    bool is_hdr = false;

    if (useWayland) {
        if (!waylandSubsurface.init(window, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, 0,
                                     nullptr, 0, nullptr)) {
            std::cerr << "Fatal: Wayland subsurface init failed" << std::endl;
            return 1;
        }
        if (!waylandSubsurface.createSwapchain(width, height)) {
            std::cerr << "Fatal: Wayland subsurface swapchain failed" << std::endl;
            return 1;
        }
        has_subsurface = true;
        is_hdr = waylandSubsurface.isHdr();
        std::cerr << "Using Wayland subsurface for video (HDR: "
                  << (is_hdr ? "yes" : "no") << ")" << std::endl;
    } else {
        // X11: No separate video layer - use OpenGL composition like Windows
        has_subsurface = false;
        is_hdr = false;
        std::cerr << "Using OpenGL composition for video (X11, no HDR)" << std::endl;
    }

    // Initialize mpv player
    // Wayland: MpvPlayerVk with subsurface for HDR support
    // X11: MpvPlayerGL with OpenGL composition (gpu-next backend)
    MpvPlayerVk mpvVk;
    MpvPlayerGL mpvGl;
    bool has_video = false;
    bool video_needs_rerender = false;
    double current_playback_rate = 1.0;

    if (useWayland) {
        if (!mpvVk.init(nullptr, &waylandSubsurface)) {
            std::cerr << "MpvPlayerVk init failed" << std::endl;
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    } else {
        if (!mpvGl.init(&egl)) {
            std::cerr << "MpvPlayerGL init failed" << std::endl;
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    // Player dispatch helpers - route to correct player based on display server
    auto mpvLoadFile = [&](const std::string& path, double startSec = 0.0) {
        return useWayland ? mpvVk.loadFile(path, startSec) : mpvGl.loadFile(path, startSec);
    };
    auto mpvStop = [&]() { useWayland ? mpvVk.stop() : mpvGl.stop(); };
    auto mpvPause = [&]() { useWayland ? mpvVk.pause() : mpvGl.pause(); };
    auto mpvPlay = [&]() { useWayland ? mpvVk.play() : mpvGl.play(); };
    auto mpvSeek = [&](double sec) { useWayland ? mpvVk.seek(sec) : mpvGl.seek(sec); };
    auto mpvSetVolume = [&](int vol) { useWayland ? mpvVk.setVolume(vol) : mpvGl.setVolume(vol); };
    auto mpvSetMuted = [&](bool m) { useWayland ? mpvVk.setMuted(m) : mpvGl.setMuted(m); };
    auto mpvSetSpeed = [&](double s) { useWayland ? mpvVk.setSpeed(s) : mpvGl.setSpeed(s); };
    auto mpvSetNormalizationGain = [&](double g) { useWayland ? mpvVk.setNormalizationGain(g) : mpvGl.setNormalizationGain(g); };
    auto mpvSetSubtitleTrack = [&](int sid) { useWayland ? mpvVk.setSubtitleTrack(sid) : mpvGl.setSubtitleTrack(sid); };
    auto mpvSetAudioTrack = [&](int aid) { useWayland ? mpvVk.setAudioTrack(aid) : mpvGl.setAudioTrack(aid); };
    auto mpvSetAudioDelay = [&](double d) { useWayland ? mpvVk.setAudioDelay(d) : mpvGl.setAudioDelay(d); };
    auto mpvIsPaused = [&]() { return useWayland ? mpvVk.isPaused() : mpvGl.isPaused(); };
    auto mpvIsPlaying = [&]() { return useWayland ? mpvVk.isPlaying() : mpvGl.isPlaying(); };
    auto mpvHasFrame = [&]() { return useWayland ? mpvVk.hasFrame() : mpvGl.hasFrame(); };
    auto mpvProcessEvents = [&]() { useWayland ? mpvVk.processEvents() : mpvGl.processEvents(); };
    auto mpvCleanup = [&]() { useWayland ? mpvVk.cleanup() : mpvGl.cleanup(); };

    // Initialize OpenGL compositor for CEF overlay
    float initial_scale = SDL_GetWindowDisplayScale(window);
    int physical_width = static_cast<int>(width * initial_scale);
    int physical_height = static_cast<int>(height * initial_scale);
    std::cerr << "[HiDPI] initial_scale=" << initial_scale
              << " logical=" << width << "x" << height
              << " physical=" << physical_width << "x" << physical_height << std::endl;

    OpenGLCompositor compositor;
    if (!compositor.init(&egl, physical_width, physical_height, use_gpu_overlay)) {
        std::cerr << "OpenGLCompositor init failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Second compositor for overlay browser
    OpenGLCompositor overlay_compositor;
    if (!overlay_compositor.init(&egl, physical_width, physical_height, false)) {
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
    settings.external_message_pump = true;
#elif defined(_WIN32)
    // Windows: multi_threaded_message_loop is supported
    settings.multi_threaded_message_loop = true;
#else
    settings.multi_threaded_message_loop = true;
#endif

#ifdef __APPLE__
    // macOS: Set framework path (cef_framework_path set earlier during CEF loading)
    CefString(&settings.framework_dir_path).FromString((cef_framework_path / "Chromium Embedded Framework.framework").string());
    // Use main executable as subprocess - it handles CefExecuteProcess early
    CefString(&settings.browser_subprocess_path).FromString((exe_path / "jellyfin-desktop-cef").string());
#elif defined(_WIN32)
    // Windows: Get exe path
    wchar_t exe_buf[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    std::filesystem::path exe_path = std::filesystem::path(exe_buf).parent_path();
    CefString(&settings.resources_dir_path).FromString(exe_path.string());
    CefString(&settings.locales_dir_path).FromString((exe_path / "locales").string());
#else
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe").parent_path();
#ifdef CEF_RESOURCES_DIR
    CefString(&settings.resources_dir_path).FromString(CEF_RESOURCES_DIR);
    CefString(&settings.locales_dir_path).FromString(CEF_RESOURCES_DIR "/locales");
#else
    CefString(&settings.resources_dir_path).FromString(exe_path.string());
    CefString(&settings.locales_dir_path).FromString((exe_path / "locales").string());
#endif
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
    // Double-buffer for paint callbacks - reduces lock contention
    struct PaintBuffer {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
        bool dirty = false;
    };
    std::array<PaintBuffer, 2> paint_buffers;
    std::atomic<int> paint_write_idx{0};  // CEF writes here
    std::mutex paint_swap_mutex;  // Only held during buffer swap
    int paint_width = 0, paint_height = 0;

    // Helper to flush paint buffer to compositor (used by both macOS and Linux paths)
    auto flushPaintBuffer = [&]() {
        std::lock_guard<std::mutex> lock(paint_swap_mutex);
        int read_idx = 1 - paint_write_idx.load(std::memory_order_acquire);
        auto& buf = paint_buffers[read_idx];
        if (buf.dirty && !buf.data.empty()) {
            void* staging = compositor.getStagingBuffer(buf.width, buf.height);
            if (staging) {
                memcpy(staging, buf.data.data(), buf.width * buf.height * 4);
                compositor.markStagingDirty();
            }
            buf.dirty = false;
        }
    };

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
#elif defined(_WIN32)
    // Windows: No media session backend for now (SMTC can be added later)
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
    float clear_color = 16.0f / 255.0f;  // #101010 until fade begins
    std::string pending_server_url;

    // Context menu overlay
    MenuOverlay menu;
    if (!menu.init()) {
        std::cerr << "Warning: Failed to init menu overlay (no font found)" << std::endl;
    }

    // Cursor state
    SDL_Cursor* current_cursor = nullptr;

    // Physical pixel size callback for HiDPI support
    auto getPhysicalSize = [window](int& w, int& h) {
        SDL_GetWindowSizeInPixels(window, &w, &h);
    };

    // Overlay browser client (for loading UI)
    CefRefPtr<OverlayClient> overlay_client(new OverlayClient(width, height,
        [&](const void* buffer, int w, int h) {
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
        },
        getPhysicalSize
    ));

    // Track who initiated fullscreen (only changes from NONE, returns to NONE on exit)
    enum class FullscreenSource { NONE, WM, CEF };
    FullscreenSource fullscreen_source = FullscreenSource::NONE;

    CefRefPtr<Client> client(new Client(width, height,
        [&](const void* buffer, int w, int h) {
            static bool first_paint = true;
            if (first_paint) {
                std::cerr << "[CEF] first paint callback: " << w << "x" << h << std::endl;
                first_paint = false;
            }
            // Write to back buffer without blocking
            int write_idx = paint_write_idx.load(std::memory_order_relaxed);
            auto& buf = paint_buffers[write_idx];
            size_t size = w * h * 4;
            if (buf.data.size() < size) {
                buf.data.resize(size);
            }
            memcpy(buf.data.data(), buffer, size);
            buf.width = w;
            buf.height = h;

            // Swap buffers (brief lock)
            {
                std::lock_guard<std::mutex> lock(paint_swap_mutex);
                buf.dirty = true;
                paint_write_idx.store(1 - write_idx, std::memory_order_release);
            }

            paint_width = w;
            paint_height = h;
        },
        [&](const std::string& cmd, const std::string& arg, int intArg, const std::string& metadata) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_cmds.push_back({cmd, arg, intArg, 0.0, metadata});
        },
#if defined(__APPLE__) || defined(_WIN32)
        nullptr,  // No GPU accelerated paint on macOS/Windows
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
        },
        getPhysicalSize
    ));

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
#if defined(__linux__) && !defined(_WIN32)
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
#ifdef __APPLE__
    // macOS: Don't pause video on minimize - CAMetalLayer continues rendering
#elif defined(_WIN32)
    MpvLayer mpv_layer(&mpv);
    window_state.add(&mpv_layer);
#else
    MpvLayerVk mpv_layer_vk(&mpvVk);
    MpvLayerGL mpv_layer_gl(&mpvGl);
    window_state.add(useWayland ? static_cast<WindowStateListener*>(&mpv_layer_vk) : static_cast<WindowStateListener*>(&mpv_layer_gl));
#endif

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
    // Callbacks for position updates
    auto positionCb = [&](double ms) {
        mediaSession.setPosition(static_cast<int64_t>(ms * 1000.0));
    };
    auto durationCb = [&](double ms) {
        client->updateDuration(ms);
    };
    auto playingCb = [&]() {
        client->emitPlaying();
        mediaSession.setPlaybackState(PlaybackState::Playing);
    };
    auto stateCb = [&](bool paused) {
        if (!mpvIsPlaying()) return;
        if (paused) {
            client->emitPaused();
            mediaSession.setPlaybackState(PlaybackState::Paused);
        } else {
            client->emitPlaying();
            mediaSession.setPlaybackState(PlaybackState::Playing);
        }
    };
    auto finishedCb = [&]() {
        std::cerr << "[MAIN] Track finished naturally (EOF), emitting finished signal" << std::endl;
        has_video = false;
#if !defined(__APPLE__) && !defined(_WIN32)
        if (has_subsurface) {
            waylandSubsurface.setVisible(false);
        }
#endif
        client->emitFinished();
        mediaSession.setPlaybackState(PlaybackState::Stopped);
    };
    auto canceledCb = [&]() {
        std::cerr << "[MAIN] Track canceled (user stop), emitting canceled signal" << std::endl;
        has_video = false;
#if !defined(__APPLE__) && !defined(_WIN32)
        if (has_subsurface) {
            waylandSubsurface.setVisible(false);
        }
#endif
        client->emitCanceled();
        mediaSession.setPlaybackState(PlaybackState::Stopped);
    };
    auto seekedCb = [&](double ms) {
        client->updatePosition(ms);
        mediaSession.setPosition(static_cast<int64_t>(ms * 1000.0));
        mediaSession.setRate(current_playback_rate);
        mediaSession.emitSeeked(static_cast<int64_t>(ms * 1000.0));
    };
    auto bufferingCb = [&](bool buffering, double ms) {
        mediaSession.setPosition(static_cast<int64_t>(ms * 1000.0));
        if (buffering) {
            mediaSession.setRate(0.0);
        } else {
            mediaSession.setRate(current_playback_rate);
        }
    };
    auto bufferedRangesCb = [&](const auto& ranges) {
        std::string json = "[";
        for (size_t i = 0; i < ranges.size(); i++) {
            if (i > 0) json += ",";
            json += "{\"start\":" + std::to_string(ranges[i].start) +
                    ",\"end\":" + std::to_string(ranges[i].end) + "}";
        }
        json += "]";
        client->executeJS("if(window._nativeUpdateBufferedRanges)window._nativeUpdateBufferedRanges(" + json + ");");
    };
    auto coreIdleCb = [&](bool idle, double ms) {
        (void)idle;
        mediaSession.setPosition(static_cast<int64_t>(ms * 1000.0));
    };
    auto errorCb = [&](const std::string& error) {
        std::cerr << "[MAIN] Playback error: " << error << std::endl;
        has_video = false;
#if !defined(__APPLE__) && !defined(_WIN32)
        if (has_subsurface) {
            waylandSubsurface.setVisible(false);
        }
#endif
        client->emitError(error);
        mediaSession.setPlaybackState(PlaybackState::Stopped);
    };

    // Set callbacks on the active player
#if defined(__APPLE__) || defined(_WIN32)
    mpv.setPositionCallback(positionCb);
    mpv.setDurationCallback(durationCb);
    mpv.setPlayingCallback(playingCb);
    mpv.setStateCallback(stateCb);
    mpv.setFinishedCallback(finishedCb);
    mpv.setCanceledCallback(canceledCb);
    mpv.setSeekedCallback(seekedCb);
    mpv.setBufferingCallback(bufferingCb);
    mpv.setBufferedRangesCallback(bufferedRangesCb);
    mpv.setCoreIdleCallback(coreIdleCb);
    mpv.setErrorCallback(errorCb);
#else
    if (useWayland) {
        mpvVk.setPositionCallback(positionCb);
        mpvVk.setDurationCallback(durationCb);
        mpvVk.setPlayingCallback(playingCb);
        mpvVk.setStateCallback(stateCb);
        mpvVk.setFinishedCallback(finishedCb);
        mpvVk.setCanceledCallback(canceledCb);
        mpvVk.setSeekedCallback(seekedCb);
        mpvVk.setBufferingCallback(bufferingCb);
        mpvVk.setBufferedRangesCallback(bufferedRangesCb);
        mpvVk.setCoreIdleCallback(coreIdleCb);
        mpvVk.setErrorCallback(errorCb);
    } else {
        mpvGl.setPositionCallback(positionCb);
        mpvGl.setDurationCallback(durationCb);
        mpvGl.setPlayingCallback(playingCb);
        mpvGl.setStateCallback(stateCb);
        mpvGl.setFinishedCallback(finishedCb);
        mpvGl.setCanceledCallback(canceledCb);
        mpvGl.setSeekedCallback(seekedCb);
        mpvGl.setBufferingCallback(bufferingCb);
        // MpvPlayerGL uses same BufferedRange struct
        mpvGl.setBufferedRangesCallback([&](const std::vector<MpvPlayerGL::BufferedRange>& ranges) {
            std::string json = "[";
            for (size_t i = 0; i < ranges.size(); i++) {
                if (i > 0) json += ",";
                json += "{\"start\":" + std::to_string(ranges[i].start) +
                        ",\"end\":" + std::to_string(ranges[i].end) + "}";
            }
            json += "]";
            client->executeJS("if(window._nativeUpdateBufferedRanges)window._nativeUpdateBufferedRanges(" + json + ");");
        });
        mpvGl.setCoreIdleCallback(coreIdleCb);
        mpvGl.setErrorCallback(errorCb);
    }
#endif

    // Auto-load test video if provided via --video
    if (!test_video.empty()) {
        std::cerr << "[TEST] Loading video: " << test_video << std::endl;
        if (mpvLoadFile(test_video)) {
            has_video = true;
#ifdef __APPLE__
            if (has_subsurface && videoLayer.isHdr()) {
                // macOS EDR is automatic
            }
#elif defined(_WIN32)
            // Windows HDR is automatic via DXGI colorspace
#else
            if (has_subsurface && is_hdr && useWayland) {
                waylandSubsurface.setColorspace();
            }
#endif
            client->emitPlaying();
        } else {
            std::cerr << "[TEST] Failed to load: " << test_video << std::endl;
        }
    }

#ifdef __APPLE__
    // Live resize support - event watcher is called during modal resize loop
    struct LiveResizeContext {
        SDL_Window* window;
        MetalCompositor* compositor;
        MetalCompositor* overlay_compositor;
        Client* client;
        OverlayClient* overlay_client;
        MacOSVideoLayer* videoLayer;
        MpvPlayerVk* mpv;
        int* current_width;
        int* current_height;
        bool* has_subsurface;
        bool* has_video;
        float* overlay_browser_alpha;
        OverlayState* overlay_state;
        // Paint buffer access for main browser
        std::array<PaintBuffer, 2>* paint_buffers;
        std::atomic<int>* paint_write_idx;
        std::mutex* paint_swap_mutex;
    };
    LiveResizeContext live_resize_ctx = {
        window,
        &compositor,
        &overlay_compositor,
        client.get(),
        overlay_client.get(),
        &videoLayer,
        &mpv,
        &current_width,
        &current_height,
        &has_subsurface,
        &has_video,
        &overlay_browser_alpha,
        &overlay_state,
        &paint_buffers,
        &paint_write_idx,
        &paint_swap_mutex
    };

    auto liveResizeCallback = [](void* userdata, SDL_Event* event) -> bool {
        auto* ctx = static_cast<LiveResizeContext*>(userdata);

        if (event->type == SDL_EVENT_WINDOW_RESIZED) {
            *ctx->current_width = event->window.data1;
            *ctx->current_height = event->window.data2;

            // Tell CEF the new size - it will repaint asynchronously
            ctx->client->resize(*ctx->current_width, *ctx->current_height);
            ctx->overlay_client->resize(*ctx->current_width, *ctx->current_height);

            // Resize video layer
            if (*ctx->has_subsurface) {
                ctx->videoLayer->resize(*ctx->current_width, *ctx->current_height);
            }
        }

        // Pump CEF and render on EXPOSED events during live resize
        if (event->type == SDL_EVENT_WINDOW_EXPOSED && event->window.data1 == 1) {
            // Pump CEF message loop - this allows CEF to deliver paint callbacks
            CefDoMessageLoopWork();

            // Render video if playing
            if (*ctx->has_video && *ctx->has_subsurface && ctx->mpv->hasFrame()) {
                VkImage sub_image;
                VkImageView sub_view;
                VkFormat sub_format;
                if (ctx->videoLayer->startFrame(&sub_image, &sub_view, &sub_format)) {
                    ctx->mpv->render(sub_image, sub_view,
                                     ctx->videoLayer->width(), ctx->videoLayer->height(),
                                     sub_format);
                    ctx->videoLayer->submitFrame();
                }
            }

            // Flush main browser paint buffer to compositor staging
            {
                std::lock_guard<std::mutex> lock(*ctx->paint_swap_mutex);
                int read_idx = 1 - ctx->paint_write_idx->load(std::memory_order_acquire);
                auto& buf = (*ctx->paint_buffers)[read_idx];
                if (buf.dirty && !buf.data.empty()) {
                    void* staging = ctx->compositor->getStagingBuffer(buf.width, buf.height);
                    if (staging) {
                        memcpy(staging, buf.data.data(), buf.width * buf.height * 4);
                        ctx->compositor->markStagingDirty();
                    }
                    buf.dirty = false;
                }
            }

            // Composite browser content
            if (ctx->compositor->hasValidOverlay() || ctx->compositor->hasPendingContent()) {
                ctx->compositor->composite(*ctx->current_width, *ctx->current_height, 1.0f);
            }

            if (*ctx->overlay_state != OverlayState::HIDDEN && *ctx->overlay_browser_alpha > 0.01f) {
                if (ctx->overlay_compositor->hasValidOverlay() || ctx->overlay_compositor->hasPendingContent()) {
                    ctx->overlay_compositor->composite(*ctx->current_width, *ctx->current_height, *ctx->overlay_browser_alpha);
                }
            }
        }

        return true;
    };

    SDL_AddEventWatch(liveResizeCallback, &live_resize_ctx);
#endif

    // Main loop - simplified (no Vulkan command buffers for main surface)
    bool running = true;
    bool needs_render = true;  // Render first frame
    while (running && !client->isClosed()) {
        auto now = Clock::now();
        bool activity_this_frame = false;

        // Process mpv events (event-driven position/state updates)
        mpvProcessEvents();

        if (!focus_set) {
            window_state.notifyFocusGained();
            focus_set = true;
        }

        // Process media session events
        mediaSession.update();

        // Event-driven: wait for events when idle, poll when active
        SDL_Event event;
        bool have_event;
#if defined(__APPLE__) || defined(_WIN32)
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
                float scale = SDL_GetWindowDisplayScale(window);
                int physical_w = static_cast<int>(current_width * scale);
                int physical_h = static_cast<int>(current_height * scale);
                compositor.resize(physical_w, physical_h);
                overlay_compositor.resize(physical_w, physical_h);
                client->resize(current_width, current_height);
                overlay_client->resize(current_width, current_height);

                if (has_subsurface) {
                    videoLayer.resize(current_width, current_height);
                }
#elif defined(_WIN32)
                // Resize WGL context
                wgl.resize(current_width, current_height);
                compositor.resize(current_width, current_height);
                overlay_compositor.resize(current_width, current_height);
                client->resize(current_width, current_height);
                overlay_client->resize(current_width, current_height);
                video_needs_rerender = true;  // Force video rerender on resize
#else
                // Resize EGL context and compositors with physical dimensions
                float scale = SDL_GetWindowDisplayScale(window);
                int physical_w = static_cast<int>(current_width * scale);
                int physical_h = static_cast<int>(current_height * scale);
                egl.resize(physical_w, physical_h);
                compositor.resize(physical_w, physical_h);
                overlay_compositor.resize(physical_w, physical_h);
                client->resize(current_width, current_height);
                overlay_client->resize(current_width, current_height);

                // Resize video layer (Wayland only - X11 uses OpenGL composition)
                if (has_subsurface) {
                    vkDeviceWaitIdle(waylandSubsurface.vkDevice());
                    waylandSubsurface.recreateSwapchain(current_width, current_height);
                }
                video_needs_rerender = true;  // Force render even when paused
#endif

                auto resize_end = std::chrono::steady_clock::now();
                std::cerr << "[" << _ms() << "ms] resize: total="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(resize_end-resize_start).count()
                          << "ms" << std::endl;
            } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                float new_scale = SDL_GetWindowDisplayScale(window);
                int physical_w = static_cast<int>(current_width * new_scale);
                int physical_h = static_cast<int>(current_height * new_scale);
                std::cerr << "[HiDPI] Scale changed to " << new_scale
                          << ", physical: " << physical_w << "x" << physical_h << std::endl;

                // Resize compositors to new physical dimensions
                compositor.resize(physical_w, physical_h);
                overlay_compositor.resize(physical_w, physical_h);

                // Notify CEF of the scale change
                if (client->browser()) {
                    client->browser()->GetHost()->WasResized();
                }
                if (overlay_client->browser()) {
                    overlay_client->browser()->GetHost()->WasResized();
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
#if defined(__APPLE__) || defined(_WIN32)
        needs_render = activity_this_frame || has_video || compositor.hasPendingContent() || overlay_state == OverlayState::FADING;
#else
        needs_render = activity_this_frame || has_video || compositor.hasPendingContent() || compositor.hasPendingDmaBuf() || overlay_state == OverlayState::FADING;
#endif

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
                        mpvSetNormalizationGain(hasGain ? normGain : 0.0);
                    } else {
                        mpvSetNormalizationGain(0.0);  // Clear any previous gain
                    }
                    if (mpvLoadFile(cmd.url, startSec)) {
                        has_video = true;
#ifdef __APPLE__
                        if (has_subsurface && videoLayer.isHdr()) {
                            // macOS EDR is automatic
                        }
#elif defined(_WIN32)
                        // Windows HDR is automatic via DXGI colorspace
#else
                        if (has_subsurface && is_hdr && useWayland) {
                            waylandSubsurface.setColorspace();
                        }
#endif
                        // Apply initial subtitle track if specified
                        int subIdx = jsonGetIntDefault(cmd.metadata, "_subIdx", -1);
                        if (subIdx >= 0) {
                            mpvSetSubtitleTrack(subIdx);
                        }
                        // Apply initial audio track if specified
                        int audioIdx = jsonGetIntDefault(cmd.metadata, "_audioIdx", -1);
                        if (audioIdx >= 0) {
                            mpvSetAudioTrack(audioIdx);
                        }
                        // mpv events will trigger state callbacks
                    } else {
                        client->emitError("Failed to load video");
                    }
                } else if (cmd.cmd == "stop") {
                    mpvStop();
                    has_video = false;
                    video_ready = false;
#if !defined(__APPLE__) && !defined(_WIN32)
                    if (has_subsurface) {
                        waylandSubsurface.setVisible(false);
                    }
#endif
                    // mpv END_FILE event will trigger finished callback
                } else if (cmd.cmd == "pause") {
                    mpvPause();
                    // mpv pause property change will trigger state callback
                } else if (cmd.cmd == "play") {
                    mpvPlay();
                    // mpv pause property change will trigger state callback
                } else if (cmd.cmd == "playpause") {
                    if (mpvIsPaused()) {
                        mpvPlay();
                    } else {
                        mpvPause();
                    }
                    // mpv pause property change will trigger state callback
                } else if (cmd.cmd == "seek") {
                    mpvSeek(static_cast<double>(cmd.intArg) / 1000.0);
                } else if (cmd.cmd == "volume") {
                    mpvSetVolume(cmd.intArg);
                } else if (cmd.cmd == "mute") {
                    mpvSetMuted(cmd.intArg != 0);
                } else if (cmd.cmd == "speed") {
                    double speed = cmd.intArg / 1000.0;
                    mpvSetSpeed(speed);
                } else if (cmd.cmd == "subtitle") {
                    mpvSetSubtitleTrack(cmd.intArg);
                } else if (cmd.cmd == "audio") {
                    mpvSetAudioTrack(cmd.intArg);
                } else if (cmd.cmd == "audioDelay") {
                    if (!cmd.metadata.empty()) {
                        try {
                            double delay = std::stod(cmd.metadata);
                            mpvSetAudioDelay(delay);
                        } catch (...) {
                            std::cerr << "[MAIN] Invalid audioDelay value: " << cmd.metadata << std::endl;
                        }
                    }
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
                clear_color = 0.0f;  // Switch to black background
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

        flushPaintBuffer();

        // Composite main browser (Metal handles its own presentation)
        // Always call composite() - it handles "no content yet" internally and uploads staging data
        if (test_video.empty() && (compositor.hasValidOverlay() || compositor.hasPendingContent())) {
            compositor.composite(current_width, current_height, 1.0f);
        }

        // Composite overlay browser (with fade alpha)
        if (overlay_state != OverlayState::HIDDEN && overlay_browser_alpha > 0.01f) {
            if (overlay_compositor.hasValidOverlay() || overlay_compositor.hasPendingContent()) {
                overlay_compositor.composite(current_width, current_height, overlay_browser_alpha);
            }
        }
#elif defined(_WIN32)
        // Windows: OpenGL mpv rendering directly to default framebuffer
        flushPaintBuffer();
        compositor.flushOverlay();

        // Clear to background color
        glClearColor(clear_color, clear_color, clear_color, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Render video first (underneath the browser UI)
        if (has_video && (mpv.hasFrame() || video_needs_rerender)) {
            mpv.render(current_width, current_height, 0);
            video_ready = true;
            video_needs_rerender = false;
        }

        // Composite main browser on top of video
        if (test_video.empty() && compositor.hasValidOverlay()) {
            compositor.composite(current_width, current_height, 1.0f);
        }

        // Composite overlay browser (with fade alpha)
        if (overlay_state != OverlayState::HIDDEN && overlay_browser_alpha > 0.01f) {
            overlay_compositor.flushOverlay();
            if (overlay_compositor.hasValidOverlay()) {
                overlay_compositor.composite(current_width, current_height, overlay_browser_alpha);
            }
        }

        wgl.swapBuffers();
#else
        // Linux: Different rendering paths for Wayland vs X11
        // Get physical dimensions for viewport (HiDPI)
        float frame_scale = SDL_GetWindowDisplayScale(window);
        int viewport_w = static_cast<int>(current_width * frame_scale);
        int viewport_h = static_cast<int>(current_height * frame_scale);

        if (useWayland) {
            // Wayland: Render video to separate Vulkan subsurface
            if (has_subsurface && ((has_video && mpvVk.hasFrame()) || video_needs_rerender)) {
                VkImage sub_image;
                VkImageView sub_view;
                VkFormat sub_format;
                if (waylandSubsurface.startFrame(&sub_image, &sub_view, &sub_format)) {
                    mpvVk.render(sub_image, sub_view,
                                waylandSubsurface.width(), waylandSubsurface.height(),
                                sub_format);
                    waylandSubsurface.submitFrame();
                    video_ready = true;
                    video_needs_rerender = false;
                }
            }

            // Import queued DMA-BUF if any (GPU path)
            compositor.importQueuedDmaBuf();

            flushPaintBuffer();
            compositor.flushOverlay();

            // Clear main surface (transparent when video ready, bg color otherwise)
            glViewport(0, 0, viewport_w, viewport_h);
            float bg_alpha = video_ready ? 0.0f : 1.0f;
            glClearColor(clear_color, clear_color, clear_color, bg_alpha);
            glClear(GL_COLOR_BUFFER_BIT);
        } else {
            // X11: OpenGL composition (like Windows) - render video and CEF to same surface
            flushPaintBuffer();
            compositor.flushOverlay();

            // Clear to background color
            glViewport(0, 0, viewport_w, viewport_h);
            glClearColor(clear_color, clear_color, clear_color, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // Render video first (underneath the browser UI)
            if (has_video && (mpvGl.hasFrame() || video_needs_rerender)) {
                mpvGl.render(viewport_w, viewport_h, 0);
                video_ready = true;
                video_needs_rerender = false;
            }
        }

        // Composite main browser (always full opacity when no video)
        if (test_video.empty() && compositor.hasValidOverlay()) {
            compositor.composite(viewport_w, viewport_h, 1.0f);
        }

        // Composite overlay browser (with fade alpha)
        if (overlay_state != OverlayState::HIDDEN && overlay_browser_alpha > 0.01f) {
            overlay_compositor.flushOverlay();
            if (overlay_compositor.hasValidOverlay()) {
                overlay_compositor.composite(viewport_w, viewport_h, overlay_browser_alpha);
            }
        }

        // Swap buffers
        egl.swapBuffers();
#endif
    }

    // Cleanup
#ifdef __APPLE__
    SDL_RemoveEventWatch(liveResizeCallback, &live_resize_ctx);
#endif
    mpvCleanup();
    compositor.cleanup();
    overlay_compositor.cleanup();
#ifdef __APPLE__
    if (has_subsurface) {
        videoLayer.cleanup();
    }
#elif defined(_WIN32)
    wgl.cleanup();
#else
    if (has_subsurface) {
        waylandSubsurface.cleanup();
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
