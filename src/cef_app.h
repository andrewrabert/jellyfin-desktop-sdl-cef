#pragma once

#include "include/cef_app.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <string>

// Custom SDL event for video playback
// Use SDL_RegisterEvents to get the event type at runtime
extern Uint32 SDL_PLAYVIDEO_EVENT;

// Store the URL to play (set by V8 handler, read by main loop)
extern std::string g_pending_video_url;

class App : public CefApp,
            public CefBrowserProcessHandler,
            public CefRenderProcessHandler {
public:
    App() = default;

    // Set before CefInitialize to enable GPU overlay (DMA-BUF shared textures)
    static void SetGpuOverlayEnabled(bool enabled) { gpu_overlay_enabled_ = enabled; }
    static bool IsGpuOverlayEnabled() { return gpu_overlay_enabled_; }

    // Check if CEF needs work done (for external_message_pump mode)
    static bool NeedsWork() { return cef_work_pending_.exchange(false); }
    static int64_t GetWorkDelay() { return cef_work_delay_ms_; }

    // CefApp
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override;

    // CefBrowserProcessHandler
    void OnContextInitialized() override;
    void OnScheduleMessagePumpWork(int64_t delay_ms) override;
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

    // CefRenderProcessHandler
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;

private:
    static inline bool gpu_overlay_enabled_ = false;
    static inline std::atomic<bool> cef_work_pending_{false};
    static inline std::atomic<int64_t> cef_work_delay_ms_{0};

    IMPLEMENT_REFCOUNTING(App);
    DISALLOW_COPY_AND_ASSIGN(App);
};

// V8 handler for native functions
class NativeV8Handler : public CefV8Handler {
public:
    NativeV8Handler(CefRefPtr<CefBrowser> browser) : browser_(browser) {}

    bool Execute(const CefString& name,
                CefRefPtr<CefV8Value> object,
                const CefV8ValueList& arguments,
                CefRefPtr<CefV8Value>& retval,
                CefString& exception) override;

private:
    CefRefPtr<CefBrowser> browser_;
    IMPLEMENT_REFCOUNTING(NativeV8Handler);
};
