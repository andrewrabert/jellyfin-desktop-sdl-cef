// Windows message-only window for CEF external_message_pump wake-ups
// PostMessage from any thread to wake the main event loop

#ifdef _WIN32

#include <windows.h>

static HWND g_message_window = nullptr;
static constexpr UINT WM_CEF_WORK = WM_USER + 1;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

void initWindowsMessageWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"JellyfinCEFMessage";
    RegisterClassExW(&wc);

    g_message_window = CreateWindowExW(
        0, L"JellyfinCEFMessage", nullptr,
        0, 0, 0, 0, 0,
        HWND_MESSAGE,  // Message-only window
        nullptr, GetModuleHandle(nullptr), nullptr);
}

void wakeWindowsEventLoop() {
    if (g_message_window) {
        PostMessageW(g_message_window, WM_CEF_WORK, 0, 0);
    }
}

void cleanupWindowsMessageWindow() {
    if (g_message_window) {
        DestroyWindow(g_message_window);
        g_message_window = nullptr;
    }
}

#endif
