# Jellyfin Desktop CEF

Minimal CEF application with SDL2 and OpenGL.

## Prerequisites

- CMake 3.19+
- SDL2 development libraries
- OpenGL development libraries
- C++17 compiler

## CEF Setup

1. Download CEF binary distribution from https://cef-builds.spotifycdn.com/index.html
   - Choose "Standard Distribution" for your platform
   - Recommended: Latest stable branch

2. Extract to `third_party/cef/`

3. Build the CEF wrapper library:
   ```bash
   cd third_party/cef
   cmake -B build
   cmake --build build --target libcef_dll_wrapper
   ```

## Build

```bash
cmake -B build -DCEF_ROOT=/path/to/cef_binary_xxx
cmake --build build
```

Or if CEF is in `third_party/cef/`:

```bash
cmake -B build
cmake --build build
```

## Run

```bash
./build/jellyfin-desktop --single-process
```

**Note:** The `--single-process` flag is currently required due to a subprocess issue. This runs CEF in single-process mode which is suitable for development.
