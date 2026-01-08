# Jellyfin Desktop CEF

Jellyfin client using CEF for web UI, mpv/libplacebo for video playback with HDR support.

## Supported Platforms

Currently:

- **Linux** - Wayland only (no X11 support)
- **macOS** - Apple Silicon and Intel

## Architecture

- **CEF** (Chromium Embedded Framework) - windowless browser for Jellyfin web UI
- **mpv + libplacebo** - gpu-next backend for video with HDR/tone-mapping
- **SDL3** - window management and input
- **Vulkan** - video rendering to Wayland subsurface (Linux) or CAMetalLayer (macOS)
- **OpenGL/EGL** - CEF overlay compositing

## Prerequisites

### Linux

- CMake 3.19+
- C++17 compiler
- SDL3
- Vulkan SDK
- OpenGL + EGL
- Wayland development libraries (wayland-client, wayland-egl, wayland-protocols)
- wayland-scanner
- libdrm
- meson (for mpv build)

### macOS

- CMake 3.19+
- C++17 compiler (Xcode)
- SDL3
- Vulkan SDK (MoltenVK)
- meson (for mpv build)

## Setup

### CEF

1. Download CEF binary distribution from https://cef-builds.spotifycdn.com/index.html
   - Choose "Standard Distribution" for your platform
   - Recommended: Latest stable branch

2. Extract or symlink to `third_party/cef/`

3. Build the CEF wrapper library:
   ```sh
   cd third_party/cef
   cmake -B build
   cmake --build build --target libcef_dll_wrapper
   ```

### mpv

mpv is built from source as a submodule with custom gpu-next patches:

```sh
git submodule update --init third_party/mpv
```

The build system handles meson configuration automatically.

## Build

```sh
cmake -B build
cmake --build build
```

## Run

```sh
./build/jellyfin-desktop
```

### Options

- `--video <path>` - Load video directly (for testing)
- `--gpu-overlay` - Enable DMA-BUF shared textures for CEF (experimental - works, but can crash the amdgpu kernel module)
