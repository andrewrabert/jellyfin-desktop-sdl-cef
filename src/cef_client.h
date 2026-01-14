#pragma once

#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_context_menu_handler.h"
#include <atomic>
#include <functional>
#include <vector>

#ifdef __APPLE__
#include <IOSurface/IOSurface.h>
#endif

class MenuOverlay;

// Message callback for player commands from renderer
// metadata is JSON string for "load" command, empty otherwise
using PlayerMessageCallback = std::function<void(const std::string& cmd, const std::string& arg, int intArg, const std::string& metadata)>;

// Cursor change callback (passes CEF cursor type)
using CursorChangeCallback = std::function<void(cef_cursor_type_t type)>;

#ifdef __APPLE__
// macOS: IOSurface callback
using IOSurfacePaintCallback = std::function<void(IOSurfaceRef surface, int width, int height)>;
#else
// Linux: DMA-BUF plane info for accelerated paint
struct DmaBufPlane {
    int fd;
    uint32_t stride;
    uint64_t offset;
    uint64_t size;
};

struct AcceleratedPaintInfo {
    int width;
    int height;
    uint64_t modifier;
    uint32_t format;  // DRM format
    std::vector<DmaBufPlane> planes;
};

using AcceleratedPaintCallback = std::function<void(const AcceleratedPaintInfo& info)>;
#endif

class Client : public CefClient, public CefRenderHandler, public CefLifeSpanHandler, public CefDisplayHandler, public CefLoadHandler, public CefContextMenuHandler {
public:
    using PaintCallback = std::function<void(const void* buffer, int width, int height)>;

#ifdef __APPLE__
    Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg = nullptr,
           IOSurfacePaintCallback on_iosurface_paint = nullptr, MenuOverlay* menu = nullptr,
           CursorChangeCallback on_cursor_change = nullptr);
#else
    Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg = nullptr,
           AcceleratedPaintCallback on_accel_paint = nullptr, MenuOverlay* menu = nullptr,
           CursorChangeCallback on_cursor_change = nullptr);
#endif

    // CefClient
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefProcessId source_process,
                                   CefRefPtr<CefProcessMessage> message) override;

    // CefDisplayHandler
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t level,
                          const CefString& message,
                          const CefString& source,
                          int line) override;
    bool OnCursorChange(CefRefPtr<CefBrowser> browser,
                        CefCursorHandle cursor,
                        cef_cursor_type_t type,
                        const CefCursorInfo& custom_cursor_info) override;

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override;
    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override;
    void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                 const RectList& dirtyRects, const void* buffer,
                 int width, int height) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList& dirtyRects,
                            const CefAcceleratedPaintInfo& info) override;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    // CefLoadHandler
    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;

    // CefContextMenuHandler
    bool RunContextMenu(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefContextMenuParams> params,
                        CefRefPtr<CefMenuModel> model,
                        CefRefPtr<CefRunContextMenuCallback> callback) override;

    bool isClosed() const { return is_closed_; }

    // Input forwarding
    void sendMouseMove(int x, int y, int modifiers);
    void sendMouseClick(int x, int y, bool down, int button, int clickCount, int modifiers);
    void sendMouseWheel(int x, int y, float deltaX, float deltaY, int modifiers);
    void sendKeyEvent(int key, bool down, int modifiers);
    void sendChar(int charCode, int modifiers);
    void sendFocus(bool focused);
    void resize(int width, int height);
    void loadUrl(const std::string& url);

    // Execute JavaScript in the browser
    void executeJS(const std::string& code);

    // Player signal helpers
    void emitPlaying();
    void emitPaused();
    void emitFinished();
    void emitCanceled();
    void emitError(const std::string& msg);
    void emitRateChanged(double rate);
    void updatePosition(double positionMs);
    void updateDuration(double durationMs);

private:
    int width_;
    int height_;
    PaintCallback on_paint_;
    PlayerMessageCallback on_player_msg_;
#ifdef __APPLE__
    IOSurfacePaintCallback on_iosurface_paint_;
#else
    AcceleratedPaintCallback on_accel_paint_;
#endif
    MenuOverlay* menu_ = nullptr;
    CursorChangeCallback on_cursor_change_;
    std::atomic<bool> is_closed_ = false;
    CefRefPtr<CefBrowser> browser_;

    // Popup (dropdown) state
    bool popup_visible_ = false;
    CefRect popup_rect_;
    std::vector<uint8_t> popup_buffer_;
    std::vector<uint8_t> composite_buffer_;  // Main view + popup blended

    IMPLEMENT_REFCOUNTING(Client);
    DISALLOW_COPY_AND_ASSIGN(Client);
};

// Simplified client for overlay browser (no player, no menu)
class OverlayClient : public CefClient, public CefRenderHandler, public CefLifeSpanHandler, public CefDisplayHandler {
public:
    using PaintCallback = std::function<void(const void* buffer, int width, int height)>;
    using LoadServerCallback = std::function<void(const std::string& url)>;

    OverlayClient(int width, int height, PaintCallback on_paint, LoadServerCallback on_load_server);

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefProcessId source_process,
                                   CefRefPtr<CefProcessMessage> message) override;

    // CefDisplayHandler
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t level,
                          const CefString& message,
                          const CefString& source,
                          int line) override;

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                 const RectList& dirtyRects, const void* buffer,
                 int width, int height) override;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    bool isClosed() const { return is_closed_; }
    void resize(int width, int height);
    void sendFocus(bool focused);
    void sendMouseMove(int x, int y, int modifiers);
    void sendMouseClick(int x, int y, bool down, int button, int clickCount, int modifiers);
    void sendMouseWheel(int x, int y, float deltaX, float deltaY, int modifiers);
    void sendKeyEvent(int key, bool down, int modifiers);
    void sendChar(int charCode, int modifiers);

private:
    int width_;
    int height_;
    PaintCallback on_paint_;
    LoadServerCallback on_load_server_;
    std::atomic<bool> is_closed_ = false;
    CefRefPtr<CefBrowser> browser_;

    IMPLEMENT_REFCOUNTING(OverlayClient);
    DISALLOW_COPY_AND_ASSIGN(OverlayClient);
};
