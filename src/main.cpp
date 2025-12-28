#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
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

#include "renderer.h"
#include "mpv_player.h"
#include "cef_app.h"
#include "cef_client.h"

// Fade constants
constexpr float FADE_DURATION_SEC = 0.3f;  // 300ms fade
constexpr float IDLE_TIMEOUT_SEC = 5.0f;   // 5 seconds before fade starts

int main(int argc, char* argv[]) {
    // CEF initialization
    CefMainArgs main_args(argc, argv);

    CefRefPtr<App> app(new App());

    // Check if this is a subprocess
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    // Parse video path argument
    std::string video_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        // Skip CEF flags
        if (arg.rfind("--", 0) == 0) continue;
        video_path = arg;
        break;
    }

    if (video_path.empty()) {
        std::cerr << "Usage: " << argv[0] << " <video-file>" << std::endl;
        return 1;
    }

    // SDL initialization
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    const int width = 1280;
    const int height = 720;

    SDL_Window* window = SDL_CreateWindow(
        "Jellyfin Desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
    );

    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // GLEW initialization
    glewExperimental = GL_TRUE;
    GLenum glew_err = glewInit();
    if (glew_err != GLEW_OK) {
        std::cerr << "glewInit failed: " << glewGetErrorString(glew_err) << std::endl;
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetSwapInterval(1);
    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;

    // Initialize renderer
    Renderer renderer;
    if (!renderer.init(width, height)) {
        std::cerr << "Renderer init failed" << std::endl;
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize mpv player
    MpvPlayer mpv;
    if (!mpv.init(width, height)) {
        std::cerr << "MpvPlayer init failed" << std::endl;
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!mpv.loadFile(video_path)) {
        std::cerr << "Failed to load: " << video_path << std::endl;
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::cout << "Playing: " << video_path << std::endl;

    // CEF settings
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = true;  // Sandbox requires separate executable
    // NOTE: Run with --single-process flag if subprocess crashes occur

    // Get executable path for resources
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe").parent_path();
    CefString(&settings.resources_dir_path).FromString(exe_path.string());
    CefString(&settings.locales_dir_path).FromString((exe_path / "locales").string());

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        std::cerr << "CefInitialize failed" << std::endl;
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Create browser
    std::atomic<bool> texture_dirty{false};
    std::mutex buffer_mutex;
    std::vector<uint8_t> paint_buffer_copy;
    int paint_width = 0, paint_height = 0;

    CefRefPtr<Client> client(new Client(width, height,
        [&](const void* buffer, int w, int h) {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            size_t size = w * h * 4;
            paint_buffer_copy.resize(size);
            memcpy(paint_buffer_copy.data(), buffer, size);
            paint_width = w;
            paint_height = h;
            texture_dirty = true;
        }
    ));

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);

    CefBrowserSettings browser_settings;
    browser_settings.background_color = 0;  // Transparent background

    // Load local HTML file
    std::string html_path = "file://" + (exe_path / "resources" / "index.html").string();
    std::cout << "Loading: " << html_path << std::endl;

    CefBrowserHost::CreateBrowser(window_info, client, html_path, browser_settings, nullptr, nullptr);

    // Activity tracking state
    using Clock = std::chrono::steady_clock;
    auto last_activity = Clock::now();
    float overlay_alpha = 1.0f;  // Start fully visible

    // Main loop
    bool running = true;
    while (running && !client->isClosed()) {
        auto now = Clock::now();
        bool activity_this_frame = false;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) running = false;

            // Track activity
            if (event.type == SDL_MOUSEMOTION ||
                event.type == SDL_MOUSEBUTTONDOWN ||
                event.type == SDL_MOUSEBUTTONUP ||
                event.type == SDL_KEYDOWN ||
                event.type == SDL_KEYUP) {
                activity_this_frame = true;
            }
        }

        if (activity_this_frame) {
            last_activity = now;
        }

        // Calculate fade
        float idle_sec = std::chrono::duration<float>(now - last_activity).count();
        float target_alpha;
        if (idle_sec < IDLE_TIMEOUT_SEC) {
            target_alpha = 1.0f;  // Visible
        } else {
            // Fade out over FADE_DURATION_SEC after idle timeout
            float fade_progress = (idle_sec - IDLE_TIMEOUT_SEC) / FADE_DURATION_SEC;
            target_alpha = std::max(0.0f, 1.0f - fade_progress);
        }
        overlay_alpha = target_alpha;

        CefDoMessageLoopWork();

        // Update CEF texture
        if (texture_dirty) {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            if (!paint_buffer_copy.empty()) {
                renderer.updateOverlayTexture(paint_buffer_copy.data(), paint_width, paint_height);
            }
            texture_dirty = false;
        }

        // Render mpv frame
        mpv.render();

        // Composite: video background, then overlay
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.renderVideo(mpv.getTexture());
        renderer.renderOverlay(overlay_alpha);

        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    CefShutdown();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
