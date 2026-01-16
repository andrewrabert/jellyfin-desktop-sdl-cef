# Development

## Quick Start

```sh
git clone https://github.com/jellyfin-labs/jellyfin-desktop-cef
cd jellyfin-desktop-cef
git submodule update --init --recursive
python3 dev/download_cef.py
cmake -B build -G Ninja
cmake --build build
./build/jellyfin-desktop-cef
```

## Web Debugger

To get browser devtools, use remote debugging:

1. Run with `--remote-debugging-port=9222`
2. Open Chromium/Chrome and navigate to `chrome://inspect/#devices`
3. Make sure "Discover Network Targets" is checked and `localhost:9222` is configured
