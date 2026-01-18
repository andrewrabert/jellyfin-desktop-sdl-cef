#include "cef/cef_client.h"
#include "ui/menu_overlay.h"
#include "settings.h"
#include "input/sdl_to_vk.h"
#include "include/cef_urlrequest.h"
#include "include/cef_parser.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <mutex>
#ifndef __APPLE__
#include <unistd.h>  // For dup()
#endif

namespace {

void doCopy(CefRefPtr<CefBrowser> browser, bool cut) {
    if (!browser) return;
    auto frame = browser->GetFocusedFrame();
    if (!frame) frame = browser->GetMainFrame();
    if (!frame) return;
    std::string js = cut ?
        R"((function() {
            const text = window.getSelection().toString();
            if (text) {
                window.jmpNative?.setClipboard?.('text/plain', btoa(text));
            }
            document.execCommand('delete');
        })();)" :
        R"((function() {
            const el = document.activeElement;
            let text = '';
            if (el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA')) {
                text = el.value.substring(el.selectionStart, el.selectionEnd);
            } else {
                text = window.getSelection().toString();
            }
            if (text) {
                window.jmpNative?.setClipboard?.('text/plain', btoa(text));
            }
        })();)";
    frame->ExecuteJavaScript(js, "", 0);
}

void doPaste(CefRefPtr<CefBrowser> browser, const char* mimeType, const void* data, size_t len) {
    if (!browser || !data || len == 0) return;
    auto frame = browser->GetFocusedFrame();
    if (!frame) frame = browser->GetMainFrame();
    if (!frame) return;
    std::string b64Data = CefBase64Encode(data, len).ToString();
    std::string b64Mime = CefBase64Encode(mimeType, strlen(mimeType)).ToString();

    std::string js = R"((function() {
        const b64 = ')" + b64Data + R"(';
        const mime = atob(')" + b64Mime + R"(');
        const binary = atob(b64);
        const bytes = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);

        // For text, use execCommand which works reliably in inputs
        if (mime.startsWith('text/')) {
            const text = new TextDecoder().decode(bytes);
            document.execCommand('insertText', false, text);
            return;
        }

        // For binary (images etc), dispatch ClipboardEvent
        const blob = new Blob([bytes], {type: mime});
        const dt = new DataTransfer();
        dt.items.add(new File([blob], 'paste', {type: mime}));
        const event = new ClipboardEvent('paste', {
            clipboardData: dt,
            bubbles: true,
            cancelable: true
        });
        document.activeElement.dispatchEvent(event);
    })();)";
    frame->ExecuteJavaScript(js, "", 0);
}

struct ClipboardData {
    std::mutex mutex;
    std::string mimeType;
    std::vector<unsigned char> data;
};
static ClipboardData g_clipboard;

const void* clipboardCallback(void*, const char* mime_type, size_t* size) {
    std::lock_guard<std::mutex> lock(g_clipboard.mutex);
    if (g_clipboard.mimeType == mime_type) {
        *size = g_clipboard.data.size();
        return g_clipboard.data.data();
    }
    *size = 0;
    return nullptr;
}

void clipboardCleanup(void*) {
    std::lock_guard<std::mutex> lock(g_clipboard.mutex);
    g_clipboard.data.clear();
    g_clipboard.mimeType.clear();
}

bool handleSetClipboard(CefRefPtr<CefListValue> args) {
    std::string mimeType = args->GetString(0).ToString();
    std::string b64 = args->GetString(1).ToString();
    CefRefPtr<CefBinaryValue> decoded = CefBase64Decode(b64);
    if (!decoded) {
        std::cerr << "[Clipboard] base64 decode failed" << std::endl;
        return true;
    }

    if (mimeType.rfind("text/", 0) == 0) {
        std::string text(decoded->GetSize(), '\0');
        decoded->GetData(text.data(), text.size(), 0);
        SDL_SetClipboardText(text.c_str());
    } else {
        {
            std::lock_guard<std::mutex> lock(g_clipboard.mutex);
            g_clipboard.mimeType = mimeType;
            g_clipboard.data.resize(decoded->GetSize());
            decoded->GetData(g_clipboard.data.data(), g_clipboard.data.size(), 0);
        }
        const char* mimeTypes[] = { mimeType.c_str() };
        SDL_SetClipboardData(clipboardCallback, clipboardCleanup, nullptr, mimeTypes, 1);
    }
    return true;
}

void handleGetClipboard(CefRefPtr<CefBrowser> browser, CefRefPtr<CefListValue> args) {
    if (!browser) return;
    std::string mimeType = args->GetString(0).ToString();
    std::string b64;
    size_t len = 0;
    void* data = SDL_GetClipboardData(mimeType.c_str(), &len);
    if (data && len > 0) {
        b64 = CefBase64Encode(data, len).ToString();
        SDL_free(data);
    }
    CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("clipboardResult");
    msg->GetArgumentList()->SetString(0, mimeType);
    msg->GetArgumentList()->SetString(1, b64);
    browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
}
} // namespace

// URL request client for server connectivity checks
class ConnectivityURLRequestClient : public CefURLRequestClient {
public:
    ConnectivityURLRequestClient(CefRefPtr<CefBrowser> browser, const std::string& originalUrl)
        : browser_(browser), original_url_(originalUrl) {}

    void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
        auto status = request->GetRequestStatus();
        auto response = request->GetResponse();

        bool success = false;
        std::string resolved_url = original_url_;

        if (status == UR_SUCCESS && response && response->GetStatus() == 200) {
            // Check if we got valid JSON with an "Id" field
            if (response_body_.find("\"Id\"") != std::string::npos) {
                success = true;
                // Use the final URL after redirects
                resolved_url = response->GetURL().ToString();
                // Strip /System/Info/Public to get base URL
                size_t pos = resolved_url.find("/System/Info/Public");
                if (pos != std::string::npos) {
                    resolved_url = resolved_url.substr(0, pos);
                }
            }
        }

        std::cerr << "[Connectivity] Request complete: " << (success ? "success" : "failed")
                  << " url=" << resolved_url << std::endl;

        // Send result back to renderer
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("serverConnectivityResult");
        msg->GetArgumentList()->SetString(0, original_url_);
        msg->GetArgumentList()->SetBool(1, success);
        msg->GetArgumentList()->SetString(2, resolved_url);
        browser_->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
    }

    void OnUploadProgress(CefRefPtr<CefURLRequest> request, int64_t current, int64_t total) override {}
    void OnDownloadProgress(CefRefPtr<CefURLRequest> request, int64_t current, int64_t total) override {}

    void OnDownloadData(CefRefPtr<CefURLRequest> request, const void* data, size_t data_length) override {
        response_body_.append(static_cast<const char*>(data), data_length);
    }

    bool GetAuthCredentials(bool isProxy, const CefString& host, int port,
                           const CefString& realm, const CefString& scheme,
                           CefRefPtr<CefAuthCallback> callback) override {
        return false;
    }

private:
    CefRefPtr<CefBrowser> browser_;
    std::string original_url_;
    std::string response_body_;

    IMPLEMENT_REFCOUNTING(ConnectivityURLRequestClient);
};

#ifdef __APPLE__
Client::Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg,
               IOSurfacePaintCallback on_iosurface_paint, MenuOverlay* menu,
               CursorChangeCallback on_cursor_change, FullscreenChangeCallback on_fullscreen_change)
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_player_msg_(std::move(on_player_msg)), on_iosurface_paint_(std::move(on_iosurface_paint)),
      menu_(menu), on_cursor_change_(std::move(on_cursor_change)),
      on_fullscreen_change_(std::move(on_fullscreen_change)) {}
#else
Client::Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg,
               AcceleratedPaintCallback on_accel_paint, MenuOverlay* menu,
               CursorChangeCallback on_cursor_change, FullscreenChangeCallback on_fullscreen_change)
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_player_msg_(std::move(on_player_msg)), on_accel_paint_(std::move(on_accel_paint)),
      menu_(menu), on_cursor_change_(std::move(on_cursor_change)),
      on_fullscreen_change_(std::move(on_fullscreen_change)) {}
#endif

bool Client::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                               cef_log_severity_t level,
                               const CefString& message,
                               const CefString& source,
                               int line) {
    std::string levelStr;
    switch (level) {
        case LOGSEVERITY_DEBUG: levelStr = "DEBUG"; break;
        case LOGSEVERITY_INFO: levelStr = "INFO"; break;
        case LOGSEVERITY_WARNING: levelStr = "WARN"; break;
        case LOGSEVERITY_ERROR: levelStr = "ERROR"; break;
        default: levelStr = "LOG"; break;
    }
    std::cerr << "[JS:" << levelStr << "] " << message.ToString() << std::endl;
    return false;  // Allow default handling too
}

bool Client::OnCursorChange(CefRefPtr<CefBrowser> browser,
                            CefCursorHandle cursor,
                            cef_cursor_type_t type,
                            const CefCursorInfo& custom_cursor_info) {
    if (on_cursor_change_) {
        on_cursor_change_(type);
    }
    return true;  // We handled it
}

void Client::OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) {
    std::cerr << "[CEF] OnFullscreenModeChange: " << (fullscreen ? "enter" : "exit") << std::endl;
    if (on_fullscreen_change_) {
        on_fullscreen_change_(fullscreen);
    }
}

bool Client::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       CefProcessId source_process,
                                       CefRefPtr<CefProcessMessage> message) {
    if (!on_player_msg_) return false;

    std::string name = message->GetName().ToString();
    CefRefPtr<CefListValue> args = message->GetArgumentList();

    std::cerr << "[IPC] Received message: " << name << std::endl;

    if (name == "playerLoad") {
        std::string url = args->GetString(0).ToString();
        int startMs = args->GetSize() > 1 ? args->GetInt(1) : 0;
        std::string metadata = args->GetSize() > 4 ? args->GetString(4).ToString() : "{}";
        on_player_msg_("load", url, startMs, metadata);
        return true;
    } else if (name == "playerStop") {
        on_player_msg_("stop", "", 0, "");
        return true;
    } else if (name == "playerPause") {
        on_player_msg_("pause", "", 0, "");
        return true;
    } else if (name == "playerPlay") {
        on_player_msg_("play", "", 0, "");
        return true;
    } else if (name == "playerSeek") {
        int ms = args->GetInt(0);
        on_player_msg_("seek", "", ms, "");
        return true;
    } else if (name == "playerSetVolume") {
        int vol = args->GetInt(0);
        on_player_msg_("volume", "", vol, "");
        return true;
    } else if (name == "playerSetMuted") {
        bool muted = args->GetBool(0);
        on_player_msg_("mute", "", muted ? 1 : 0, "");
        return true;
    } else if (name == "playerSetSpeed") {
        int rateX1000 = args->GetInt(0);
        on_player_msg_("speed", "", rateX1000, "");
        return true;
    } else if (name == "saveServerUrl") {
        std::string url = args->GetString(0).ToString();
        std::cerr << "[IPC] Saving server URL: " << url << std::endl;
        Settings::instance().setServerUrl(url);
        Settings::instance().save();
        return true;
    } else if (name == "notifyMetadata") {
        std::string metadata = args->GetString(0).ToString();
        on_player_msg_("media_metadata", metadata, 0, "");
        return true;
    } else if (name == "notifyPosition") {
        int posMs = args->GetInt(0);
        on_player_msg_("media_position", "", posMs, "");
        return true;
    } else if (name == "notifySeek") {
        int posMs = args->GetInt(0);
        on_player_msg_("media_seeked", "", posMs, "");
        return true;
    } else if (name == "notifyPlaybackState") {
        std::string state = args->GetString(0).ToString();
        on_player_msg_("media_state", state, 0, "");
        return true;
    } else if (name == "notifyArtwork") {
        std::string artworkUri = args->GetString(0).ToString();
        on_player_msg_("media_artwork", artworkUri, 0, "");
        return true;
    } else if (name == "notifyQueueChange") {
        bool canNext = args->GetBool(0);
        bool canPrev = args->GetBool(1);
        // Encode both bools in intArg: bit 0 = canNext, bit 1 = canPrev
        int flags = (canNext ? 1 : 0) | (canPrev ? 2 : 0);
        on_player_msg_("media_queue", "", flags, "");
        return true;
    } else if (name == "notifyRateChange") {
        double rate = args->GetDouble(0);
        // Use the rate * 1000000 to pass as int (microseconds precision equivalent)
        // We'll decode this in main.cpp
        on_player_msg_("media_notify_rate", "", static_cast<int>(rate * 1000000), "");
        return true;
    } else if (name == "setClipboard") {
        return handleSetClipboard(args);
    } else if (name == "getClipboard") {
        handleGetClipboard(browser, args);
        return true;
    }

    return false;
}

void Client::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    rect.Set(0, 0, width_, height_);
}

bool Client::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) {
    // Force 1.0 scale factor to avoid Retina 2x rendering
    screen_info.device_scale_factor = 1.0f;
    screen_info.depth = 32;
    screen_info.depth_per_component = 8;
    screen_info.is_monochrome = false;
    screen_info.rect = CefRect(0, 0, width_, height_);
    screen_info.available_rect = screen_info.rect;
    return true;
}

void Client::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) {
    popup_visible_ = show;
    if (!show) {
        popup_buffer_.clear();
    }
}

void Client::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) {
    popup_rect_ = rect;
}

void Client::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                     const RectList& dirtyRects, const void* buffer,
                     int width, int height) {
    if (!on_paint_) return;

    if (type == PET_POPUP) {
        // Store popup buffer for compositing
        size_t size = width * height * 4;
        popup_buffer_.resize(size);
        memcpy(popup_buffer_.data(), buffer, size);
        // Request main view repaint to composite popup
        if (browser) {
            browser->GetHost()->Invalidate(PET_VIEW);
        }
        return;
    }

    // PET_VIEW - main view
    // Fast path: no popup, pass buffer directly (zero extra copies)
    if (!popup_visible_ || popup_buffer_.empty()) {
        on_paint_(buffer, width, height);
        return;
    }

    // Slow path: blend popup onto view (only when dropdown is visible)
    size_t size = width * height * 4;
    composite_buffer_.resize(size);
    memcpy(composite_buffer_.data(), buffer, size);

    int px = popup_rect_.x;
    int py = popup_rect_.y;
    int pw = popup_rect_.width;
    int ph = popup_rect_.height;
    for (int y = 0; y < ph; y++) {
        int dst_y = py + y;
        if (dst_y < 0 || dst_y >= height) continue;
        for (int x = 0; x < pw; x++) {
            int dst_x = px + x;
            if (dst_x < 0 || dst_x >= width) continue;
            int src_i = (y * pw + x) * 4;
            int dst_i = (dst_y * width + dst_x) * 4;
            if (src_i + 3 >= static_cast<int>(popup_buffer_.size())) continue;
            uint8_t alpha = popup_buffer_[src_i + 3];
            if (alpha == 255) {
                composite_buffer_[dst_i + 0] = popup_buffer_[src_i + 0];
                composite_buffer_[dst_i + 1] = popup_buffer_[src_i + 1];
                composite_buffer_[dst_i + 2] = popup_buffer_[src_i + 2];
                composite_buffer_[dst_i + 3] = 255;
            } else if (alpha > 0) {
                uint8_t inv = 255 - alpha;
                composite_buffer_[dst_i + 0] = (popup_buffer_[src_i + 0] * alpha + composite_buffer_[dst_i + 0] * inv) / 255;
                composite_buffer_[dst_i + 1] = (popup_buffer_[src_i + 1] * alpha + composite_buffer_[dst_i + 1] * inv) / 255;
                composite_buffer_[dst_i + 2] = (popup_buffer_[src_i + 2] * alpha + composite_buffer_[dst_i + 2] * inv) / 255;
                composite_buffer_[dst_i + 3] = 255;
            }
        }
    }
    on_paint_(composite_buffer_.data(), width, height);
}

void Client::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                const RectList& dirtyRects,
                                const CefAcceleratedPaintInfo& info) {
#ifdef __APPLE__
    // macOS: IOSurface path
    if (on_iosurface_paint_ && info.shared_texture_io_surface) {
        on_iosurface_paint_(static_cast<IOSurfaceRef>(info.shared_texture_io_surface), width_, height_);
    }
#else
    // Linux: DMA-BUF path
    if (on_accel_paint_) {
        AcceleratedPaintInfo paintInfo;
        paintInfo.width = width_;
        paintInfo.height = height_;
        paintInfo.modifier = info.modifier;
        paintInfo.format = info.format;

        for (int i = 0; i < info.plane_count; i++) {
            DmaBufPlane plane;
            plane.fd = dup(info.planes[i].fd);  // Duplicate before CEF releases it
            plane.stride = info.planes[i].stride;
            plane.offset = info.planes[i].offset;
            plane.size = info.planes[i].size;
            paintInfo.planes.push_back(plane);
        }

        on_accel_paint_(paintInfo);
    }
#endif
}

void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    browser_ = browser;
    std::cerr << "Browser created" << std::endl;
}

void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    std::cerr << "Browser closing" << std::endl;
    browser_ = nullptr;
    is_closed_ = true;
}

void Client::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) {
    if (frame->IsMain()) {
        // Set focus after page load for proper visual focus on autofocus elements
        browser->GetHost()->SetFocus(true);
    }
}

void Client::sendMouseMove(int x, int y, int modifiers) {
    if (!browser_) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    browser_->GetHost()->SendMouseMoveEvent(event, false);
}

void Client::sendMouseClick(int x, int y, bool down, int button, int clickCount, int modifiers) {
    if (!browser_) return;
    std::cerr << "[Mouse] Button " << button << " " << (down ? "DOWN" : "UP") << " at " << x << "," << y << " clicks=" << clickCount << std::endl;
    CefMouseEvent event;
    event.x = x;
    event.y = y;

    CefBrowserHost::MouseButtonType btn_type;
    switch (button) {
        case 1:
            btn_type = MBT_LEFT;
            if (down) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
            break;
        case 2:
            btn_type = MBT_MIDDLE;
            if (down) modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
            break;
        case 3:
            btn_type = MBT_RIGHT;
            if (down) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
            break;
        default:
            btn_type = MBT_LEFT;
            if (down) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
            break;
    }
    event.modifiers = modifiers;

    browser_->GetHost()->SendMouseClickEvent(event, btn_type, !down, clickCount);
}

void Client::sendKeyEvent(int key, bool down, int modifiers) {
    if (!browser_) return;

    CefKeyEvent event;
    event.windows_key_code = sdlKeyToWindowsVK(key);
#ifdef __APPLE__
    event.native_key_code = sdlKeyToMacNative(key);
#else
    event.native_key_code = key;
#endif
    event.modifiers = modifiers;
    event.type = down ? KEYEVENT_KEYDOWN : KEYEVENT_KEYUP;
    browser_->GetHost()->SendKeyEvent(event);

    // Send CHAR event for Enter key to trigger form submission
    if (down && key == 0x0D) {
        event.type = KEYEVENT_CHAR;
        event.character = '\r';
        event.unmodified_character = '\r';
        browser_->GetHost()->SendKeyEvent(event);
    }
}

void Client::sendChar(int charCode, int modifiers) {
    if (!browser_) return;
    CefKeyEvent event;
    event.windows_key_code = charCode;
    event.character = charCode;
    event.unmodified_character = charCode;
    event.type = KEYEVENT_CHAR;
    event.modifiers = modifiers;
    browser_->GetHost()->SendKeyEvent(event);
}

void Client::sendMouseWheel(int x, int y, float deltaX, float deltaY, int modifiers) {
    if (!browser_) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    // SDL3 provides smooth scroll values, scale for CEF (expects ~120 per notch)
    int pixelX = static_cast<int>(deltaX * 53.0f);  // Smooth scroll factor
    int pixelY = static_cast<int>(deltaY * 53.0f);
    browser_->GetHost()->SendMouseWheelEvent(event, pixelX, pixelY);
}

void Client::sendFocus(bool focused) {
    if (!browser_) return;
    browser_->GetHost()->SetFocus(focused);
}

void Client::sendTouch(int id, float x, float y, float radiusX, float radiusY,
                       float pressure, int type, int modifiers) {
    if (!browser_) return;
    CefTouchEvent event;
    event.id = id;
    event.x = x;
    event.y = y;
    event.radius_x = radiusX;
    event.radius_y = radiusY;
    event.rotation_angle = 0;
    event.pressure = pressure;
    event.type = static_cast<cef_touch_event_type_t>(type);
    event.modifiers = modifiers;
    event.pointer_type = CEF_POINTER_TYPE_TOUCH;
    browser_->GetHost()->SendTouchEvent(event);
}

void Client::paste(const char* mimeType, const void* data, size_t len) {
    doPaste(browser_, mimeType, data, len);
}

void Client::copy() {
    doCopy(browser_, false);
}

void Client::cut() {
    doCopy(browser_, true);
}

void Client::selectAll() {
    if (browser_) browser_->GetMainFrame()->SelectAll();
}

void Client::undo() {
    if (browser_) browser_->GetMainFrame()->Undo();
}

void Client::redo() {
    if (browser_) browser_->GetMainFrame()->Redo();
}

void Client::resize(int width, int height) {
    width_ = width;
    height_ = height;
    if (browser_) {
        browser_->GetHost()->WasResized();
    }
}

void Client::loadUrl(const std::string& url) {
    if (browser_) {
        browser_->GetMainFrame()->LoadURL(url);
    }
}

void Client::executeJS(const std::string& code) {
    if (!browser_) return;
    CefRefPtr<CefFrame> frame = browser_->GetMainFrame();
    if (frame) {
        frame->ExecuteJavaScript(code, frame->GetURL(), 0);
    }
}

void Client::exitFullscreen() {
    if (!browser_) return;
    browser_->GetHost()->ExitFullscreen(true);
}

void Client::emitPlaying() {
    executeJS("if(window._nativeEmit) window._nativeEmit('playing');");
}

void Client::emitPaused() {
    executeJS("if(window._nativeEmit) window._nativeEmit('paused');");
}

void Client::emitFinished() {
    executeJS("if(window._nativeEmit) window._nativeEmit('finished');");
}

void Client::emitCanceled() {
    executeJS("if(window._nativeEmit) window._nativeEmit('canceled');");
}

void Client::emitError(const std::string& msg) {
    executeJS("if(window._nativeEmit) window._nativeEmit('error', '" + msg + "');");
}

void Client::emitRateChanged(double rate) {
    executeJS("if(window._nativeSetRate) window._nativeSetRate(" + std::to_string(rate) + ");");
}

void Client::updatePosition(double positionMs) {
    executeJS("if(window._nativeUpdatePosition) window._nativeUpdatePosition(" + std::to_string(positionMs) + ");");
}

void Client::updateDuration(double durationMs) {
    executeJS("if(window._nativeUpdateDuration) window._nativeUpdateDuration(" + std::to_string(durationMs) + ");");
}

bool Client::RunContextMenu(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefContextMenuParams> params,
                            CefRefPtr<CefMenuModel> model,
                            CefRefPtr<CefRunContextMenuCallback> callback) {
    std::cerr << "[ContextMenu] RunContextMenu called, items=" << model->GetCount()
              << " pos=" << params->GetXCoord() << "," << params->GetYCoord()
              << " menu_=" << (menu_ ? "yes" : "no") << std::endl;

    if (!menu_ || model->GetCount() == 0) {
        std::cerr << "[ContextMenu] Cancelled (no menu or no items)" << std::endl;
        callback->Cancel();
        return true;
    }

    // Build menu items
    std::vector<MenuItem> items;
    for (size_t i = 0; i < model->GetCount(); i++) {
        if (model->GetTypeAt(i) == MENUITEMTYPE_SEPARATOR) continue;
        std::string label = model->GetLabelAt(i).ToString();
        if (label.empty()) continue;
        items.push_back({
            model->GetCommandIdAt(i),
            label,
            model->IsEnabledAt(i)
        });
    }

    if (items.empty()) {
        callback->Cancel();
        return true;
    }

    std::cerr << "[ContextMenu] Opening menu with " << items.size() << " items" << std::endl;
    menu_->open(params->GetXCoord(), params->GetYCoord(), items, callback);
    return true;
}

// OverlayClient implementation
OverlayClient::OverlayClient(int width, int height, PaintCallback on_paint, LoadServerCallback on_load_server)
    : width_(width), height_(height), on_paint_(std::move(on_paint)), on_load_server_(std::move(on_load_server)) {}

bool OverlayClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                      cef_log_severity_t level,
                                      const CefString& message,
                                      const CefString& source,
                                      int line) {
    std::string levelStr;
    switch (level) {
        case LOGSEVERITY_DEBUG: levelStr = "DEBUG"; break;
        case LOGSEVERITY_INFO: levelStr = "INFO"; break;
        case LOGSEVERITY_WARNING: levelStr = "WARN"; break;
        case LOGSEVERITY_ERROR: levelStr = "ERROR"; break;
        default: levelStr = "LOG"; break;
    }
    std::cerr << "[Overlay:" << levelStr << "] " << message.ToString() << std::endl;
    return false;
}

bool OverlayClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                              CefRefPtr<CefFrame> frame,
                                              CefProcessId source_process,
                                              CefRefPtr<CefProcessMessage> message) {
    std::string name = message->GetName().ToString();
    CefRefPtr<CefListValue> args = message->GetArgumentList();

    std::cerr << "[Overlay IPC] Received: " << name << std::endl;

    if (name == "loadServer" && on_load_server_) {
        std::string url = args->GetString(0).ToString();
        on_load_server_(url);
        return true;
    }

    if (name == "saveServerUrl") {
        std::string url = args->GetString(0).ToString();
        std::cerr << "[Overlay IPC] Saving server URL: " << url << std::endl;
        Settings::instance().setServerUrl(url);
        Settings::instance().save();
        return true;
    }

    if (name == "checkServerConnectivity") {
        std::string url = args->GetString(0).ToString();
        std::cerr << "[Overlay IPC] Checking connectivity: " << url << std::endl;

        // Normalize URL
        if (url.find("://") == std::string::npos) {
            url = "http://" + url;
        }
        // Remove trailing slash
        if (!url.empty() && url.back() == '/') {
            url.pop_back();
        }

        std::string check_url = url + "/System/Info/Public";

        CefRefPtr<CefRequest> request = CefRequest::Create();
        request->SetURL(check_url);
        request->SetMethod("GET");

        CefRefPtr<ConnectivityURLRequestClient> client =
            new ConnectivityURLRequestClient(browser, url);
        CefURLRequest::Create(request, client, nullptr);
        return true;
    }

    if (name == "setClipboard") {
        return handleSetClipboard(args);
    }

    if (name == "getClipboard") {
        handleGetClipboard(browser, args);
        return true;
    }

    std::cerr << "[Overlay IPC] Unhandled: " << name << std::endl;
    return false;
}

void OverlayClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    rect.Set(0, 0, width_, height_);
}

bool OverlayClient::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) {
    // Force 1.0 scale factor to avoid Retina 2x rendering
    screen_info.device_scale_factor = 1.0f;
    screen_info.depth = 32;
    screen_info.depth_per_component = 8;
    screen_info.is_monochrome = false;
    screen_info.rect = CefRect(0, 0, width_, height_);
    screen_info.available_rect = screen_info.rect;
    return true;
}

void OverlayClient::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                             const RectList& dirtyRects, const void* buffer,
                             int width, int height) {
    if (on_paint_ && type == PET_VIEW) {
        on_paint_(buffer, width, height);
    }
}

void OverlayClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    browser_ = browser;
    std::cerr << "Overlay browser created" << std::endl;
}

void OverlayClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    std::cerr << "Overlay browser closing" << std::endl;
    browser_ = nullptr;
    is_closed_ = true;
}

void OverlayClient::resize(int width, int height) {
    width_ = width;
    height_ = height;
    if (browser_) {
        browser_->GetHost()->WasResized();
    }
}

void OverlayClient::sendFocus(bool focused) {
    if (browser_) browser_->GetHost()->SetFocus(focused);
}

void OverlayClient::sendMouseMove(int x, int y, int modifiers) {
    if (!browser_) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    browser_->GetHost()->SendMouseMoveEvent(event, false);
}

void OverlayClient::sendMouseClick(int x, int y, bool down, int button, int clickCount, int modifiers) {
    if (!browser_) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;

    CefBrowserHost::MouseButtonType btn_type;
    switch (button) {
        case 1: btn_type = MBT_LEFT; if (down) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON; break;
        case 2: btn_type = MBT_MIDDLE; if (down) modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON; break;
        case 3: btn_type = MBT_RIGHT; if (down) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON; break;
        default: btn_type = MBT_LEFT; if (down) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON; break;
    }
    event.modifiers = modifiers;
    browser_->GetHost()->SendMouseClickEvent(event, btn_type, !down, clickCount);
}

void OverlayClient::sendMouseWheel(int x, int y, float deltaX, float deltaY, int modifiers) {
    if (!browser_) return;
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    int pixelX = static_cast<int>(deltaX * 53.0f);
    int pixelY = static_cast<int>(deltaY * 53.0f);
    browser_->GetHost()->SendMouseWheelEvent(event, pixelX, pixelY);
}

void OverlayClient::sendKeyEvent(int key, bool down, int modifiers) {
    if (!browser_) return;
    CefKeyEvent event;
    event.windows_key_code = sdlKeyToWindowsVK(key);
#ifdef __APPLE__
    event.native_key_code = sdlKeyToMacNative(key);
#else
    event.native_key_code = key;
#endif
    event.modifiers = modifiers;
    event.type = down ? KEYEVENT_KEYDOWN : KEYEVENT_KEYUP;
    browser_->GetHost()->SendKeyEvent(event);

    if (down && key == 0x0D) {
        event.type = KEYEVENT_CHAR;
        event.character = '\r';
        event.unmodified_character = '\r';
        browser_->GetHost()->SendKeyEvent(event);
    }
}

void OverlayClient::sendChar(int charCode, int modifiers) {
    if (!browser_) return;
    CefKeyEvent event;
    event.windows_key_code = charCode;
    event.character = charCode;
    event.unmodified_character = charCode;
    event.type = KEYEVENT_CHAR;
    event.modifiers = modifiers;
    browser_->GetHost()->SendKeyEvent(event);
}

void OverlayClient::sendTouch(int id, float x, float y, float radiusX, float radiusY,
                              float pressure, int type, int modifiers) {
    if (!browser_) return;
    CefTouchEvent event;
    event.id = id;
    event.x = x;
    event.y = y;
    event.radius_x = radiusX;
    event.radius_y = radiusY;
    event.rotation_angle = 0;
    event.pressure = pressure;
    event.type = static_cast<cef_touch_event_type_t>(type);
    event.modifiers = modifiers;
    event.pointer_type = CEF_POINTER_TYPE_TOUCH;
    browser_->GetHost()->SendTouchEvent(event);
}

void OverlayClient::paste(const char* mimeType, const void* data, size_t len) {
    doPaste(browser_, mimeType, data, len);
}

void OverlayClient::copy() {
    doCopy(browser_, false);
}

void OverlayClient::cut() {
    doCopy(browser_, true);
}

void OverlayClient::selectAll() {
    if (browser_) browser_->GetMainFrame()->SelectAll();
}

void OverlayClient::undo() {
    if (browser_) browser_->GetMainFrame()->Undo();
}

void OverlayClient::redo() {
    if (browser_) browser_->GetMainFrame()->Redo();
}
