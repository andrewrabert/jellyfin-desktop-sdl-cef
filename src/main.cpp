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

// Double/triple click detection
constexpr int MULTI_CLICK_DISTANCE = 4;    // Max pixels between clicks
constexpr Uint32 MULTI_CLICK_TIME = 500;   // Max ms between clicks

// Convert SDL modifier state to CEF modifier flags
int sdlModsToCef(Uint16 sdlMods) {
    int cef = 0;
    if (sdlMods & KMOD_SHIFT) cef |= (1 << 1);  // EVENTFLAG_SHIFT_DOWN
    if (sdlMods & KMOD_CTRL)  cef |= (1 << 2);  // EVENTFLAG_CONTROL_DOWN
    if (sdlMods & KMOD_ALT)   cef |= (1 << 3);  // EVENTFLAG_ALT_DOWN
    return cef;
}

int main(int argc, char* argv[]) {
    // CEF initialization
    CefMainArgs main_args(argc, argv);

    CefRefPtr<App> app(new App());

    // Check if this is a subprocess
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    // Parse optional video path argument
    std::string video_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        // Skip CEF flags
        if (arg.rfind("--", 0) == 0) continue;
        video_path = arg;
        break;
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
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
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

    // Enable text input for keyboard events
    SDL_StartTextInput();

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
    bool has_video = false;
    if (!mpv.init(width, height)) {
        std::cerr << "MpvPlayer init failed" << std::endl;
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }


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

    // Player command queue (filled by IPC callback, processed in main loop)
    struct PlayerCmd {
        std::string cmd;
        std::string url;
        int intArg;
    };
    std::mutex cmd_mutex;
    std::vector<PlayerCmd> pending_cmds;

    CefRefPtr<Client> client(new Client(width, height,
        // Paint callback
        [&](const void* buffer, int w, int h) {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            size_t size = w * h * 4;
            paint_buffer_copy.resize(size);
            memcpy(paint_buffer_copy.data(), buffer, size);
            paint_width = w;
            paint_height = h;
            texture_dirty = true;
        },
        // Player message callback (from renderer IPC)
        [&](const std::string& cmd, const std::string& arg, int intArg) {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            pending_cmds.push_back({cmd, arg, intArg});
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

    // Mouse position tracking for scroll events
    int mouse_x = 0, mouse_y = 0;

    // Multi-click tracking
    Uint32 last_click_time = 0;
    int last_click_x = 0, last_click_y = 0;
    int last_click_button = 0;
    int click_count = 0;

    // Set initial focus
    bool focus_set = false;

    // Main loop
    bool running = true;
    while (running && !client->isClosed()) {
        auto now = Clock::now();
        bool activity_this_frame = false;

        // Set focus once browser is ready
        if (!focus_set) {
            client->sendFocus(true);
            focus_set = true;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) running = false;


            // Track activity
            if (event.type == SDL_MOUSEMOTION ||
                event.type == SDL_MOUSEBUTTONDOWN ||
                event.type == SDL_MOUSEBUTTONUP ||
                event.type == SDL_MOUSEWHEEL ||
                event.type == SDL_KEYDOWN ||
                event.type == SDL_KEYUP) {
                activity_this_frame = true;
            }

            // Get current modifier state
            int mods = sdlModsToCef(SDL_GetModState());

            // Forward input to CEF
            if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
                client->sendMouseMove(mouse_x, mouse_y, mods);
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                int x = event.button.x;
                int y = event.button.y;
                int btn = event.button.button;
                Uint32 now_ticks = SDL_GetTicks();

                // Detect multi-click
                int dx = x - last_click_x;
                int dy = y - last_click_y;
                bool same_spot = (dx * dx + dy * dy) <= (MULTI_CLICK_DISTANCE * MULTI_CLICK_DISTANCE);
                bool same_button = (btn == last_click_button);
                bool in_time = (now_ticks - last_click_time) <= MULTI_CLICK_TIME;

                if (same_spot && same_button && in_time) {
                    click_count = (click_count % 3) + 1;  // Cycle 1 -> 2 -> 3 -> 1
                } else {
                    click_count = 1;
                }

                last_click_time = now_ticks;
                last_click_x = x;
                last_click_y = y;
                last_click_button = btn;

                client->sendMouseClick(x, y, true, btn, click_count, mods);
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                client->sendMouseClick(event.button.x, event.button.y, false, event.button.button, click_count, mods);
            } else if (event.type == SDL_MOUSEWHEEL) {
                // SDL2: y > 0 = scroll up, y < 0 = scroll down
                client->sendMouseWheel(mouse_x, mouse_y, event.wheel.x, event.wheel.y, mods);
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    client->sendFocus(true);
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    client->sendFocus(false);
                } else if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    int new_w = event.window.data1;
                    int new_h = event.window.data2;
                    glViewport(0, 0, new_w, new_h);
                    renderer.resize(new_w, new_h);
                    client->resize(new_w, new_h);
                }
            } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                bool down = (event.type == SDL_KEYDOWN);
                SDL_Keycode key = event.key.keysym.sym;
                // Forward control keys and modified keys (Ctrl+C, etc.)
                bool is_control_key = (key == SDLK_BACKSPACE || key == SDLK_DELETE ||
                    key == SDLK_RETURN || key == SDLK_ESCAPE ||
                    key == SDLK_TAB || key == SDLK_LEFT || key == SDLK_RIGHT ||
                    key == SDLK_UP || key == SDLK_DOWN || key == SDLK_HOME ||
                    key == SDLK_END || key == SDLK_PAGEUP || key == SDLK_PAGEDOWN ||
                    key == SDLK_F5 || key == SDLK_F11);
                bool has_ctrl = (mods & (1 << 2)) != 0;
                // Forward if control key OR if Ctrl is held (for Ctrl+C, etc.)
                if (is_control_key || has_ctrl) {
                    client->sendKeyEvent(key, down, mods);
                }
            } else if (event.type == SDL_TEXTINPUT) {
                // UTF-8 text input - handles all printable characters
                for (const char* c = event.text.text; *c; ++c) {
                    client->sendChar(static_cast<unsigned char>(*c), mods);
                }
            }
        }

        if (activity_this_frame) {
            last_activity = now;
        }

        // Process player commands from IPC
        {
            std::lock_guard<std::mutex> lock(cmd_mutex);
            for (const auto& cmd : pending_cmds) {
                if (cmd.cmd == "load") {
                    std::cout << "[MAIN] playerLoad: " << cmd.url << std::endl;
                    if (mpv.loadFile(cmd.url)) {
                        has_video = true;
                        std::cout << "[MAIN] Video loaded, emitting playing signal" << std::endl;
                        client->emitPlaying();
                    } else {
                        std::cerr << "[MAIN] Load failed" << std::endl;
                        client->emitError("Failed to load video");
                    }
                } else if (cmd.cmd == "stop") {
                    std::cout << "[MAIN] playerStop" << std::endl;
                    mpv.stop();
                    has_video = false;
                    client->emitFinished();
                } else if (cmd.cmd == "pause") {
                    std::cout << "[MAIN] playerPause" << std::endl;
                    mpv.pause();
                    client->emitPaused();
                } else if (cmd.cmd == "play") {
                    std::cout << "[MAIN] playerPlay" << std::endl;
                    mpv.play();
                    client->emitPlaying();
                } else if (cmd.cmd == "seek") {
                    std::cout << "[MAIN] playerSeek: " << cmd.intArg << "ms" << std::endl;
                    mpv.seek(static_cast<double>(cmd.intArg) / 1000.0);
                } else if (cmd.cmd == "volume") {
                    std::cout << "[MAIN] playerSetVolume: " << cmd.intArg << std::endl;
                    mpv.setVolume(cmd.intArg);
                } else if (cmd.cmd == "mute") {
                    std::cout << "[MAIN] playerSetMuted: " << cmd.intArg << std::endl;
                    mpv.setMuted(cmd.intArg != 0);
                }
            }
            pending_cmds.clear();
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

        // Update position/duration to JS periodically when playing
        static auto last_position_update = Clock::now();
        if (has_video && mpv.isPlaying()) {
            auto time_since_update = std::chrono::duration<float>(now - last_position_update).count();
            if (time_since_update >= 0.5f) {  // Update every 500ms
                double pos = mpv.getPosition() * 1000.0;  // seconds -> ms
                double dur = mpv.getDuration() * 1000.0;
                client->updatePosition(pos);
                if (dur > 0) {
                    client->updateDuration(dur);
                }
                last_position_update = now;
            }
        }

        // Update CEF texture
        if (texture_dirty) {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            if (!paint_buffer_copy.empty()) {
                renderer.updateOverlayTexture(paint_buffer_copy.data(), paint_width, paint_height);
            }
            texture_dirty = false;
        }

        // Composite rendering
        glClear(GL_COLOR_BUFFER_BIT);
        if (has_video) {
            mpv.render();
            renderer.renderVideo(mpv.getTexture());
            renderer.renderOverlay(overlay_alpha);
        } else {
            renderer.renderOverlay(1.0f);  // Full opacity, no fade without video
        }

        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    CefShutdown();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
