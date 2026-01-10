# Jellyfin Desktop CEF

Jellyfin client using CEF for web UI, mpv/libplacebo for video playback with HDR support.

## Supported Platforms

- **Linux** - Wayland only (no X11 support)
- **macOS** - Apple Silicon and Intel

## Architecture

- **CEF** (Chromium Embedded Framework) - windowless browser for Jellyfin web UI
- **mpv + libplacebo** - gpu-next backend for video with HDR/tone-mapping
- **SDL3** - window management and input
- **Vulkan** - video rendering to Wayland subsurface (Linux) or CAMetalLayer (macOS)
- **OpenGL/EGL** - CEF overlay compositing

## Building

See [dev/](dev/README.md) for build instructions.

## Options

- `--video <path>` - Load video directly (for testing)
- `--gpu-overlay` - Enable DMA-BUF shared textures for CEF (experimental)
- `--remote-debugging-port=<port>` - Enable Chrome DevTools
