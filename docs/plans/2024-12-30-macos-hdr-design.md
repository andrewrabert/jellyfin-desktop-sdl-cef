# macOS HDR Support Design

## Summary

Add macOS support with HDR video playback and GPU-accelerated CEF overlay, matching Linux performance.

## Architecture

```
┌─────────────────────────────────────────┐
│             NSWindow                     │
│  ┌─────────────────────────────────────┐│
│  │  CEF Layer (CAOpenGLLayer - SDR)    ││
│  │  - OpenGL compositor (existing)      ││
│  │  - IOSurface import via CGL          ││
│  └─────────────────────────────────────┘│
│  ┌─────────────────────────────────────┐│
│  │  Video Layer (CAMetalLayer - HDR)   ││
│  │  - MoltenVK swapchain                ││
│  │  - mpv/libplacebo renders here       ││
│  │  - EDR enabled for HDR               ││
│  └─────────────────────────────────────┘│
└─────────────────────────────────────────┘
```

CEF layer above video layer. macOS compositor handles SDR/HDR blending correctly.

## Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Graphics API | MoltenVK | Reuse existing Vulkan code |
| CEF texture sharing | IOSurface | Zero-copy, CEF exports this on macOS |
| HDR output | Layered CAMetalLayers | Avoids colorspace blending issues |
| Platform code | Separate files | Clean separation, minimal #ifdef |
| CEF layer renderer | OpenGL via SDL | Quickest path, reuses existing code |

## File Structure

```
src/
├── main.cpp                    # #ifdef APPLE vs LINUX for setup
├── opengl_compositor.cpp/h     # Shared (add IOSurface import path)
├── vulkan_compositor.cpp/h     # Shared (mpv rendering)
├── vulkan_context.cpp/h        # Shared (MoltenVK works)
│
├── wayland_subsurface.cpp/h    # Linux only
├── egl_context.cpp/h           # Linux only
│
├── macos_layer.mm/h            # macOS only - CAMetalLayer setup
└── cgl_context.mm/h            # macOS only - CGL for OpenGL
```

## CEF IOSurface Integration

On macOS, CEF's `OnAcceleratedPaint` provides IOSurfaceRef instead of DMA-BUF.

```cpp
// opengl_compositor.cpp
#ifdef __APPLE__
bool OpenGLCompositor::updateOverlayFromIOSurface(IOSurfaceRef surface) {
    glBindTexture(GL_TEXTURE_RECTANGLE, texture_);
    CGLTexImageIOSurface2D(cgl_context_, GL_TEXTURE_RECTANGLE,
                           GL_RGBA, width_, height_,
                           GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                           surface, 0);
    return true;
}
#endif
```

Uses `GL_TEXTURE_RECTANGLE` (macOS requirement). Shader needs rectangle sampler.

## macOS HDR Video Layer

```objc
// macos_layer.mm
CAMetalLayer *layer = [CAMetalLayer layer];
layer.wantsExtendedDynamicRangeContent = YES;
layer.pixelFormat = MTLPixelFormatRGBA16Float;
layer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
```

MoltenVK creates swapchain against CAMetalLayer. libplacebo handles HDR output.

## Implementation Order

1. CGL context (~50 lines)
2. OpenGL compositor IOSurface path (~30 lines)
3. MacOSVideoLayer (~150 lines)
4. main.cpp platform split (~50 lines)
5. CMake platform detection (~20 lines)
6. CEF callback split (~10 lines)

**Total: ~300 lines new macOS code**

## Dependencies

- MoltenVK (Vulkan SDK or homebrew)
- CEF macOS build
- SDL3 (cross-platform)
- Frameworks: Cocoa, Metal, QuartzCore, OpenGL
