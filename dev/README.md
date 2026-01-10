# Development

## Quick Start

```sh
# Download CEF
python3 dev/download_cef.py

# Build CEF wrapper
cd third_party/cef && cmake -B build && cmake --build build --target libcef_dll_wrapper && cd ../..

# Init mpv submodule
git submodule update --init third_party/mpv

# Build and run
cmake -B build && cmake --build build
./build/jellyfin-desktop
```

## CEF Download

```sh
python3 dev/download_cef.py
```

Options:
- `--show-latest` - Show latest version info as JSON
- `--platform <platform>` - linux64, linuxarm64, macosx64, macosarm64
- `--version <version>` - Specific CEF version

## Web Debugger

To get browser devtools, use remote debugging:

1. Run with `--remote-debugging-port=9222`
2. Open Chromium/Chrome and navigate to `chrome://inspect/#devices`
3. Make sure "Discover Network Targets" is checked and `localhost:9222` is configured
