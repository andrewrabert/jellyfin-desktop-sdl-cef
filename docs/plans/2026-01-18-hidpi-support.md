# HiDPI Support Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make CEF respect host display scale factor for crisp rendering on HiDPI displays.

**Architecture:** Pass a scale factor callback from main.cpp to Client/OverlayClient classes. `GetScreenInfo()` calls this callback to get current scale. CEF renders at `logical_size * scale`, compositor receives scaled buffer and renders to viewport. Mouse coordinates from SDL are already logical - no input changes needed.

**Tech Stack:** SDL3 (`SDL_GetWindowDisplayScale`), CEF (`GetScreenInfo`, `device_scale_factor`)

---

## Background

Current state:
- `GetScreenInfo()` in both `Client` and `OverlayClient` force `device_scale_factor = 1.0f`
- On HiDPI displays, web content appears blurry/pixelated
- SDL3 provides `SDL_GetWindowDisplayScale(window)` which returns the actual scale

Key insight:
- `GetViewRect()` returns logical dimensions (unchanged)
- `GetScreenInfo()` sets `device_scale_factor` to actual scale
- CEF's `OnPaint` provides buffers at `logical_width * scale` x `logical_height * scale`
- Compositor already handles variable buffer sizes - just needs to accept larger textures

---

### Task 1: Add Scale Factor Callback Type and Member to Client

**Files:**
- Modify: `src/cef/cef_client.h:41` (after existing callbacks)
- Modify: `src/cef/cef_client.h:77-86` (constructors)
- Modify: `src/cef/cef_client.h:174-186` (members)

**Step 1: Add callback type alias**

In `src/cef/cef_client.h`, after line 47 (after `FullscreenChangeCallback`), add:

```cpp
// Scale factor query callback (returns display scale, e.g., 2.0 for Retina)
using ScaleFactorCallback = std::function<float()>;
```

**Step 2: Add scale_factor_ member to Client**

In `src/cef/cef_client.h`, after line 186 (`CefRefPtr<CefBrowser> browser_;`), add:

```cpp
    ScaleFactorCallback scale_factor_cb_;
```

**Step 3: Update Client constructor declarations (both platforms)**

In `src/cef/cef_client.h`, update the macOS constructor (lines 77-80) to:

```cpp
    Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg = nullptr,
           IOSurfacePaintCallback on_iosurface_paint = nullptr, MenuOverlay* menu = nullptr,
           CursorChangeCallback on_cursor_change = nullptr,
           FullscreenChangeCallback on_fullscreen_change = nullptr,
           ScaleFactorCallback scale_factor_cb = nullptr);
```

Update the Linux constructor (lines 82-85) to:

```cpp
    Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg = nullptr,
           AcceleratedPaintCallback on_accel_paint = nullptr, MenuOverlay* menu = nullptr,
           CursorChangeCallback on_cursor_change = nullptr,
           FullscreenChangeCallback on_fullscreen_change = nullptr,
           ScaleFactorCallback scale_factor_cb = nullptr);
```

**Step 4: Commit**

```bash
git add src/cef/cef_client.h
git commit -m "feat(hidpi): add ScaleFactorCallback type and member to Client"
```

---

### Task 2: Add Scale Factor Callback to OverlayClient

**Files:**
- Modify: `src/cef/cef_client.h:206` (constructor)
- Modify: `src/cef/cef_client.h:251-257` (members)

**Step 1: Update OverlayClient constructor declaration**

In `src/cef/cef_client.h`, update line 206 to:

```cpp
    OverlayClient(int width, int height, PaintCallback on_paint, LoadServerCallback on_load_server,
                  ScaleFactorCallback scale_factor_cb = nullptr);
```

**Step 2: Add scale_factor_ member to OverlayClient**

In `src/cef/cef_client.h`, after line 256 (`CefRefPtr<CefBrowser> browser_;`), add:

```cpp
    ScaleFactorCallback scale_factor_cb_;
```

**Step 3: Commit**

```bash
git add src/cef/cef_client.h
git commit -m "feat(hidpi): add ScaleFactorCallback to OverlayClient"
```

---

### Task 3: Update Client Constructor Implementation

**Files:**
- Modify: `src/cef/cef_client.cpp:205-221` (both constructor implementations)

**Step 1: Update macOS constructor implementation**

In `src/cef/cef_client.cpp`, replace lines 206-212:

```cpp
Client::Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg,
               IOSurfacePaintCallback on_iosurface_paint, MenuOverlay* menu,
               CursorChangeCallback on_cursor_change, FullscreenChangeCallback on_fullscreen_change,
               ScaleFactorCallback scale_factor_cb)
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_player_msg_(std::move(on_player_msg)), on_iosurface_paint_(std::move(on_iosurface_paint)),
      menu_(menu), on_cursor_change_(std::move(on_cursor_change)),
      on_fullscreen_change_(std::move(on_fullscreen_change)),
      scale_factor_cb_(std::move(scale_factor_cb)) {}
```

**Step 2: Update Linux constructor implementation**

In `src/cef/cef_client.cpp`, replace lines 214-220:

```cpp
Client::Client(int width, int height, PaintCallback on_paint, PlayerMessageCallback on_player_msg,
               AcceleratedPaintCallback on_accel_paint, MenuOverlay* menu,
               CursorChangeCallback on_cursor_change, FullscreenChangeCallback on_fullscreen_change,
               ScaleFactorCallback scale_factor_cb)
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_player_msg_(std::move(on_player_msg)), on_accel_paint_(std::move(on_accel_paint)),
      menu_(menu), on_cursor_change_(std::move(on_cursor_change)),
      on_fullscreen_change_(std::move(on_fullscreen_change)),
      scale_factor_cb_(std::move(scale_factor_cb)) {}
```

**Step 3: Commit**

```bash
git add src/cef/cef_client.cpp
git commit -m "feat(hidpi): update Client constructor implementations"
```

---

### Task 4: Update OverlayClient Constructor Implementation

**Files:**
- Modify: `src/cef/cef_client.cpp:734-735`

**Step 1: Update OverlayClient constructor**

In `src/cef/cef_client.cpp`, replace lines 734-735:

```cpp
OverlayClient::OverlayClient(int width, int height, PaintCallback on_paint, LoadServerCallback on_load_server,
                             ScaleFactorCallback scale_factor_cb)
    : width_(width), height_(height), on_paint_(std::move(on_paint)),
      on_load_server_(std::move(on_load_server)),
      scale_factor_cb_(std::move(scale_factor_cb)) {}
```

**Step 2: Commit**

```bash
git add src/cef/cef_client.cpp
git commit -m "feat(hidpi): update OverlayClient constructor implementation"
```

---

### Task 5: Update Client::GetScreenInfo to Use Scale Factor

**Files:**
- Modify: `src/cef/cef_client.cpp:373-382`

**Step 1: Update GetScreenInfo implementation**

In `src/cef/cef_client.cpp`, replace lines 373-382:

```cpp
bool Client::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) {
    float scale = scale_factor_cb_ ? scale_factor_cb_() : 1.0f;
    screen_info.device_scale_factor = scale;
    screen_info.depth = 32;
    screen_info.depth_per_component = 8;
    screen_info.is_monochrome = false;
    screen_info.rect = CefRect(0, 0, width_, height_);
    screen_info.available_rect = screen_info.rect;
    return true;
}
```

**Step 2: Commit**

```bash
git add src/cef/cef_client.cpp
git commit -m "feat(hidpi): Client::GetScreenInfo uses scale factor callback"
```

---

### Task 6: Update OverlayClient::GetScreenInfo to Use Scale Factor

**Files:**
- Modify: `src/cef/cef_client.cpp:819-828`

**Step 1: Update GetScreenInfo implementation**

In `src/cef/cef_client.cpp`, replace lines 819-828:

```cpp
bool OverlayClient::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) {
    float scale = scale_factor_cb_ ? scale_factor_cb_() : 1.0f;
    screen_info.device_scale_factor = scale;
    screen_info.depth = 32;
    screen_info.depth_per_component = 8;
    screen_info.is_monochrome = false;
    screen_info.rect = CefRect(0, 0, width_, height_);
    screen_info.available_rect = screen_info.rect;
    return true;
}
```

**Step 2: Commit**

```bash
git add src/cef/cef_client.cpp
git commit -m "feat(hidpi): OverlayClient::GetScreenInfo uses scale factor callback"
```

---

### Task 7: Pass Scale Factor Callback from main.cpp

**Files:**
- Modify: `src/main.cpp:565` (OverlayClient creation)
- Modify: `src/main.cpp:596` (Client creation)

**Step 1: Create scale factor lambda**

In `src/main.cpp`, before line 565 (before OverlayClient creation), add:

```cpp
    // Scale factor callback for HiDPI support
    auto getScaleFactor = [window]() -> float {
        return SDL_GetWindowDisplayScale(window);
    };
```

**Step 2: Pass callback to OverlayClient**

In `src/main.cpp`, update the OverlayClient construction (around line 565-590) to pass the callback as the last argument. Find:

```cpp
    CefRefPtr<OverlayClient> overlay_client(new OverlayClient(width, height,
```

After the LoadServerCallback lambda (ending around line 589), add the scale callback:

```cpp
        },
        getScaleFactor
    ));
```

**Step 3: Pass callback to Client**

In `src/main.cpp`, update the Client construction (around line 596-663) to pass the callback as the last argument. Find the fullscreen callback lambda (ending around line 662), and add after it:

```cpp
        },
        getScaleFactor
    ));
```

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat(hidpi): pass scale factor callback to Client and OverlayClient"
```

---

### Task 8: Update Compositor to Handle Scaled Buffer Dimensions

**Files:**
- Modify: `src/compositor/opengl_compositor.cpp:306-311`

The compositor's `getStagingBuffer()` currently rejects buffers that don't match compositor dimensions. With HiDPI, CEF sends `width * scale` x `height * scale` buffers. We need the compositor to accept these larger buffers.

**Step 1: Update getStagingBuffer to accept scaled dimensions**

In `src/compositor/opengl_compositor.cpp`, replace the getStagingBuffer function (lines 306-311):

```cpp
void* OpenGLCompositor::getStagingBuffer(int width, int height) {
    // Accept buffers that are scaled versions of our logical size
    // CEF sends width*scale x height*scale for HiDPI
    size_t required_size = static_cast<size_t>(width) * height * 4;
    size_t current_size = static_cast<size_t>(width_) * height_ * 4;

    // Reject if buffer is smaller than our allocation or doesn't fit PBO
    if (required_size > current_size || !pbo_mapped_) {
        return nullptr;
    }
    return pbo_mapped_;
}
```

Wait - this approach is wrong. The compositor's texture and PBO are allocated at logical size. If CEF sends a 2x buffer, we can't fit it.

**Revised approach:** The compositor dimensions should track the *physical* pixel dimensions (width * scale, height * scale), not logical. But this requires larger changes.

**Simpler approach:** Resize compositor to match CEF buffer dimensions when they arrive. The compositor already has `resize()` method.

Actually, the cleanest fix: **resize the compositor to physical dimensions upfront** when we know the scale.

Let me revise this task.

**Step 1: Track physical dimensions in compositor**

The compositor's `width_` and `height_` should be physical pixels. In `main.cpp`, we should init and resize the compositor with `width * scale` and `height * scale`.

In `src/main.cpp`, update compositor initialization (around line 397-399 for Linux):

Find:
```cpp
    OpenGLCompositor compositor;
    if (!compositor.init(&egl, width, height, use_gpu_overlay)) {
```

Replace with:
```cpp
    float initial_scale = SDL_GetWindowDisplayScale(window);
    int physical_width = static_cast<int>(width * initial_scale);
    int physical_height = static_cast<int>(height * initial_scale);

    OpenGLCompositor compositor;
    if (!compositor.init(&egl, physical_width, physical_height, use_gpu_overlay)) {
```

And similarly for overlay_compositor (around line 406):
```cpp
    OpenGLCompositor overlay_compositor;
    if (!overlay_compositor.init(&egl, physical_width, physical_height, false)) {
```

**Step 2: Update resize handling**

In `src/main.cpp`, find the resize handling (around lines 982-995). Update to use physical dimensions:

Find:
```cpp
                compositor.resize(current_width, current_height);
                overlay_compositor.resize(current_width, current_height);
```

Replace with:
```cpp
                float scale = SDL_GetWindowDisplayScale(window);
                int physical_w = static_cast<int>(current_width * scale);
                int physical_h = static_cast<int>(current_height * scale);
                compositor.resize(physical_w, physical_h);
                overlay_compositor.resize(physical_w, physical_h);
```

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat(hidpi): initialize and resize compositor with physical dimensions"
```

---

### Task 9: Update macOS Metal Compositor Initialization (if applicable)

**Files:**
- Modify: `src/main.cpp:343-358` (macOS Metal compositor init)

**Step 1: Update macOS compositor initialization**

In `src/main.cpp`, find the macOS compositor initialization (around lines 343-358). Update to use physical dimensions:

Find:
```cpp
    MetalCompositor compositor;
    if (!compositor.init(window, width, height)) {
```

Replace with:
```cpp
    float initial_scale = SDL_GetWindowDisplayScale(window);
    int physical_width = static_cast<int>(width * initial_scale);
    int physical_height = static_cast<int>(height * initial_scale);

    MetalCompositor compositor;
    if (!compositor.init(window, physical_width, physical_height)) {
```

And for overlay_compositor:
```cpp
    MetalCompositor overlay_compositor;
    if (!overlay_compositor.init(window, physical_width, physical_height)) {
```

**Step 2: Update macOS resize handling**

In `src/main.cpp`, find the macOS resize block (around lines 972-976). Update:

Find:
```cpp
                compositor.resize(current_width, current_height);
                overlay_compositor.resize(current_width, current_height);
```

Replace with:
```cpp
                float scale = SDL_GetWindowDisplayScale(window);
                int physical_w = static_cast<int>(current_width * scale);
                int physical_h = static_cast<int>(current_height * scale);
                compositor.resize(physical_w, physical_h);
                overlay_compositor.resize(physical_w, physical_h);
```

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat(hidpi): macOS Metal compositor uses physical dimensions"
```

---

### Task 10: Build and Manual Test

**Step 1: Build the project**

```bash
cmake --build build
```

**Step 2: Verify build succeeds**

Expected: Build completes without errors.

**Step 3: Test on HiDPI display**

Run the application on a HiDPI display (or set `GDK_SCALE=2` on Linux for testing):

```bash
GDK_SCALE=2 ./build/jellyfin-desktop
```

Expected behavior:
- Web content should appear sharp/crisp
- Text should not be blurry
- UI should be properly sized (not tiny or huge)

**Step 4: Test on standard display**

Run without scale override:

```bash
./build/jellyfin-desktop
```

Expected: Behavior unchanged from before (scale factor = 1.0).

**Step 5: Commit (if any fixes needed)**

```bash
git add -A
git commit -m "fix(hidpi): address issues found in manual testing"
```

---

### Task 11: Handle Display Scale Changes at Runtime

**Files:**
- Modify: `src/main.cpp` (event loop, SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED)

SDL3 provides `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` when the scale factor changes (e.g., window moved between displays).

**Step 1: Add handler for scale change event**

In `src/main.cpp`, in the event handling section (around line 930-1001), add a new case after `SDL_EVENT_WINDOW_RESIZED`:

```cpp
            } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                float new_scale = SDL_GetWindowDisplayScale(window);
                int physical_w = static_cast<int>(current_width * new_scale);
                int physical_h = static_cast<int>(current_height * new_scale);
                std::cerr << "[HiDPI] Scale changed to " << new_scale
                          << ", physical: " << physical_w << "x" << physical_h << std::endl;

                // Resize compositors to new physical dimensions
#ifdef __APPLE__
                compositor.resize(physical_w, physical_h);
                overlay_compositor.resize(physical_w, physical_h);
#else
                compositor.resize(physical_w, physical_h);
                overlay_compositor.resize(physical_w, physical_h);
#endif

                // Notify CEF of the scale change
                if (client->browser()) {
                    client->browser()->GetHost()->WasResized();
                }
                if (overlay_client->browser()) {
                    overlay_client->browser()->GetHost()->WasResized();
                }
            }
```

**Step 2: Add browser() accessor to Client and OverlayClient**

In `src/cef/cef_client.h`, add to Client class (around line 138, after `isClosed()`):

```cpp
    CefRefPtr<CefBrowser> browser() const { return browser_; }
```

In `src/cef/cef_client.h`, add to OverlayClient class (around line 234, after `isClosed()`):

```cpp
    CefRefPtr<CefBrowser> browser() const { return browser_; }
```

**Step 3: Commit**

```bash
git add src/main.cpp src/cef/cef_client.h
git commit -m "feat(hidpi): handle display scale changes at runtime"
```

---

### Task 12: Final Build and Integration Test

**Step 1: Clean build**

```bash
rm -rf build && cmake -B build && cmake --build build
```

**Step 2: Test complete workflow**

1. Launch on HiDPI display
2. Verify sharp rendering
3. Move window to different display (if available)
4. Verify scale adjusts correctly
5. Resize window, verify still sharp

**Step 3: Create final commit if needed**

```bash
git add -A
git commit -m "feat(hidpi): complete HiDPI support implementation"
```

---

## Summary

This implementation:
1. Adds a `ScaleFactorCallback` that queries `SDL_GetWindowDisplayScale()`
2. Passes this callback to `Client` and `OverlayClient` at construction
3. `GetScreenInfo()` uses the callback to return actual scale factor
4. CEF renders at physical dimensions (`logical * scale`)
5. Compositors are initialized and resized to physical dimensions
6. Runtime scale changes are handled via `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`

No input coordinate changes needed - SDL already provides logical coordinates.
