#include "cef_client.h"
#include "menu_overlay.h"
#include "settings.h"
#include <iostream>
#ifndef __APPLE__
#include <unistd.h>  // For dup()
#endif

#ifdef __APPLE__
Client::Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg,
               IOSurfacePaintCallback on_iosurface_paint, MenuOverlay* menu,
               CursorChangeCallback on_cursor_change)
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_player_msg_(std::move(on_player_msg)), on_iosurface_paint_(std::move(on_iosurface_paint)),
      menu_(menu), on_cursor_change_(std::move(on_cursor_change)) {}
#else
Client::Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg,
               AcceleratedPaintCallback on_accel_paint, MenuOverlay* menu,
               CursorChangeCallback on_cursor_change)
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_player_msg_(std::move(on_player_msg)), on_accel_paint_(std::move(on_accel_paint)),
      menu_(menu), on_cursor_change_(std::move(on_cursor_change)) {}
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
    } else if (name == "setFullscreen") {
        bool enable = args->GetBool(0);
        on_player_msg_("fullscreen", "", enable ? 1 : 0, "");
        return true;
    } else if (name == "notifyMetadata") {
        std::string metadata = args->GetString(0).ToString();
        on_player_msg_("mpris_metadata", metadata, 0, "");
        return true;
    } else if (name == "notifyPosition") {
        int posMs = args->GetInt(0);
        on_player_msg_("mpris_position", "", posMs, "");
        return true;
    } else if (name == "notifySeek") {
        int posMs = args->GetInt(0);
        on_player_msg_("mpris_seeked", "", posMs, "");
        return true;
    } else if (name == "notifyPlaybackState") {
        std::string state = args->GetString(0).ToString();
        on_player_msg_("mpris_state", state, 0, "");
        return true;
    } else if (name == "notifyArtwork") {
        std::string artworkUri = args->GetString(0).ToString();
        on_player_msg_("mpris_artwork", artworkUri, 0, "");
        return true;
    } else if (name == "notifyQueueChange") {
        bool canNext = args->GetBool(0);
        bool canPrev = args->GetBool(1);
        // Encode both bools in intArg: bit 0 = canNext, bit 1 = canPrev
        int flags = (canNext ? 1 : 0) | (canPrev ? 2 : 0);
        on_player_msg_("mpris_queue", "", flags, "");
        return true;
    } else if (name == "notifyRateChange") {
        double rate = args->GetDouble(0);
        // Use the rate * 1000000 to pass as int (microseconds precision equivalent)
        // We'll decode this in main.cpp
        on_player_msg_("mpris_notify_rate", "", static_cast<int>(rate * 1000000), "");
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

// Map SDL keycodes to Windows virtual key codes (used by CEF cross-platform)
static int sdlKeyToWindows(int sdlKey) {
    switch (sdlKey) {
        case 0x08: return 0x08;  // SDLK_BACKSPACE -> VK_BACK
        case 0x09: return 0x09;  // SDLK_TAB -> VK_TAB
        case 0x0D: return 0x0D;  // SDLK_RETURN -> VK_RETURN
        case 0x1B: return 0x1B;  // SDLK_ESCAPE -> VK_ESCAPE
        case 0x20: return 0x20;  // SDLK_SPACE -> VK_SPACE
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
        // Letters for Ctrl/Cmd+key combos (SDL uses lowercase)
        case 'a': return 'A';
        case 'c': return 'C';
        case 'v': return 'V';
        case 'x': return 'X';
        case 'z': return 'Z';
        case 'y': return 'Y';
        default: return sdlKey;
    }
}

#ifdef __APPLE__
// Map SDL keycodes to Mac virtual key codes (Carbon kVK_* values)
static int sdlKeyToMacNative(int sdlKey) {
    switch (sdlKey) {
        case 0x08: return 0x33;  // SDLK_BACKSPACE -> kVK_Delete
        case 0x09: return 0x30;  // SDLK_TAB -> kVK_Tab
        case 0x0D: return 0x24;  // SDLK_RETURN -> kVK_Return
        case 0x1B: return 0x35;  // SDLK_ESCAPE -> kVK_Escape
        case 0x20: return 0x31;  // SDLK_SPACE -> kVK_Space
        case 0x7F: return 0x75;  // SDLK_DELETE -> kVK_ForwardDelete
        case 0x40000050: return 0x7B;  // SDLK_LEFT -> kVK_LeftArrow
        case 0x4000004F: return 0x7C;  // SDLK_RIGHT -> kVK_RightArrow
        case 0x40000052: return 0x7E;  // SDLK_UP -> kVK_UpArrow
        case 0x40000051: return 0x7D;  // SDLK_DOWN -> kVK_DownArrow
        case 0x4000004A: return 0x73;  // SDLK_HOME -> kVK_Home
        case 0x4000004D: return 0x77;  // SDLK_END -> kVK_End
        case 0x4000004B: return 0x74;  // SDLK_PAGEUP -> kVK_PageUp
        case 0x4000004E: return 0x79;  // SDLK_PAGEDOWN -> kVK_PageDown
        case 0x4000003A: return 0x60;  // SDLK_F5 -> kVK_F5
        case 0x40000044: return 0x67;  // SDLK_F11 -> kVK_F11
        // Letter keys (lowercase ASCII to Mac key codes)
        case 'a': return 0x00;  // kVK_ANSI_A
        case 'c': return 0x08;  // kVK_ANSI_C
        case 'v': return 0x09;  // kVK_ANSI_V
        case 'x': return 0x07;  // kVK_ANSI_X
        case 'z': return 0x06;  // kVK_ANSI_Z
        case 'y': return 0x10;  // kVK_ANSI_Y
        default: return sdlKey;
    }
}
#endif

void Client::sendKeyEvent(int key, bool down, int modifiers) {
    if (!browser_) return;
    CefKeyEvent event;
    event.windows_key_code = sdlKeyToWindows(key);
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
    event.windows_key_code = sdlKeyToWindows(key);
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
