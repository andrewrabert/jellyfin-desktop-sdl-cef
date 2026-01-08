# Project Notes

## Build
```
cmake --build build
```

## Architecture
- CEF (Chromium Embedded Framework) for web UI
- mpv via libmpv for video playback
- Vulkan rendering with libplacebo (gpu-next backend)
- Wayland subsurface for video layer

## mpv Integration
- Custom libmpv gpu-next path in `third_party/mpv/video/out/gpu_next/`
- `video.c` - main rendering, uses `map_scaler()` for proper filtering
- `context.c` - Vulkan FBO wrapping for libmpv
- `libmpv_gpu_next.c` - render backend glue
