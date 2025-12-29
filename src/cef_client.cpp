#include "cef_client.h"
#include "settings.h"
#include <iostream>
#include <unistd.h>  // For dup()

Client::Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg,
               AcceleratedPaintCallback on_accel_paint)
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_player_msg_(std::move(on_player_msg)), on_accel_paint_(std::move(on_accel_paint)) {}

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
    std::cout << "[JS:" << levelStr << "] " << message.ToString() << std::endl;
    return false;  // Allow default handling too
}

bool Client::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       CefProcessId source_process,
                                       CefRefPtr<CefProcessMessage> message) {
    if (!on_player_msg_) return false;

    std::string name = message->GetName().ToString();
    CefRefPtr<CefListValue> args = message->GetArgumentList();

    std::cout << "[IPC] Received message: " << name << std::endl;

    if (name == "playerLoad") {
        std::string url = args->GetString(0).ToString();
        int startMs = args->GetSize() > 1 ? args->GetInt(1) : 0;
        on_player_msg_("load", url, startMs);
        return true;
    } else if (name == "playerStop") {
        on_player_msg_("stop", "", 0);
        return true;
    } else if (name == "playerPause") {
        on_player_msg_("pause", "", 0);
        return true;
    } else if (name == "playerPlay") {
        on_player_msg_("play", "", 0);
        return true;
    } else if (name == "playerSeek") {
        int ms = args->GetInt(0);
        on_player_msg_("seek", "", ms);
        return true;
    } else if (name == "playerSetVolume") {
        int vol = args->GetInt(0);
        on_player_msg_("volume", "", vol);
        return true;
    } else if (name == "playerSetMuted") {
        bool muted = args->GetBool(0);
        on_player_msg_("mute", "", muted ? 1 : 0);
        return true;
    } else if (name == "saveServerUrl") {
        std::string url = args->GetString(0).ToString();
        std::cout << "[IPC] Saving server URL: " << url << std::endl;
        Settings::instance().setServerUrl(url);
        Settings::instance().save();
        return true;
    } else if (name == "setFullscreen") {
        bool enable = args->GetBool(0);
        on_player_msg_("fullscreen", "", enable ? 1 : 0);
        return true;
    }

    return false;
}

void Client::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    rect.Set(0, 0, width_, height_);
}

void Client::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                     const RectList& dirtyRects, const void* buffer,
                     int width, int height) {
    if (on_paint_) {
        on_paint_(buffer, width, height);
    }
}

void Client::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                const RectList& dirtyRects,
                                const CefAcceleratedPaintInfo& info) {
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
}

void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    browser_ = browser;
    std::cout << "Browser created" << std::endl;
}

void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    std::cout << "Browser closing" << std::endl;
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

// Map SDL keycodes to Windows virtual key codes
static int sdlKeyToWindows(int sdlKey) {
    switch (sdlKey) {
        case 0x08: return 0x08;  // SDLK_BACKSPACE -> VK_BACK
        case 0x09: return 0x09;  // SDLK_TAB -> VK_TAB
        case 0x0D: return 0x0D;  // SDLK_RETURN -> VK_RETURN
        case 0x1B: return 0x1B;  // SDLK_ESCAPE -> VK_ESCAPE
        case 0x7F: return 0x2E;  // SDLK_DELETE -> VK_DELETE
        case 0x40000050: return 0x25;  // SDLK_LEFT -> VK_LEFT
        case 0x4000004F: return 0x27;  // SDLK_RIGHT -> VK_RIGHT
        case 0x40000052: return 0x26;  // SDLK_UP -> VK_UP
        case 0x40000051: return 0x28;  // SDLK_DOWN -> VK_DOWN
        case 0x4000004A: return 0x24;  // SDLK_HOME -> VK_HOME
        case 0x4000004D: return 0x23;  // SDLK_END -> VK_END
        case 0x4000004B: return 0x21;  // SDLK_PAGEUP -> VK_PRIOR
        case 0x4000004E: return 0x22;  // SDLK_PAGEDOWN -> VK_NEXT
        case 0x4000003A: return 0x74;  // SDLK_F5 -> VK_F5
        case 0x40000044: return 0x7A;  // SDLK_F11 -> VK_F11
        // Letters for Ctrl+key combos (SDL uses lowercase)
        case 'a': return 'A';
        case 'c': return 'C';
        case 'v': return 'V';
        case 'x': return 'X';
        case 'z': return 'Z';
        case 'y': return 'Y';
        default: return sdlKey;
    }
}

void Client::sendKeyEvent(int key, bool down, int modifiers) {
    if (!browser_) return;
    CefKeyEvent event;
    event.windows_key_code = sdlKeyToWindows(key);
    event.native_key_code = key;
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

void Client::resize(int width, int height) {
    width_ = width;
    height_ = height;
    if (browser_) {
        browser_->GetHost()->WasResized();
    }
}

void Client::executeJS(const std::string& code) {
    if (!browser_) return;
    CefRefPtr<CefFrame> frame = browser_->GetMainFrame();
    if (frame) {
        frame->ExecuteJavaScript(code, frame->GetURL(), 0);
    }
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

void Client::emitError(const std::string& msg) {
    executeJS("if(window._nativeEmit) window._nativeEmit('error', '" + msg + "');");
}

void Client::updatePosition(double positionMs) {
    executeJS("if(window._nativeUpdatePosition) window._nativeUpdatePosition(" + std::to_string(positionMs) + ");");
}

void Client::updateDuration(double durationMs) {
    executeJS("if(window._nativeUpdateDuration) window._nativeUpdateDuration(" + std::to_string(durationMs) + ");");
}
