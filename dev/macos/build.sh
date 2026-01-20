#!/usr/bin/env sh
# Jellyfin Desktop CEF - macOS build script
# Run setup.sh first to install dependencies
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Check dependencies
if ! command -v cmake > /dev/null; then
    echo "error: cmake not found. Run setup.sh first" >&2
    exit 1
fi

if ! command -v ninja > /dev/null; then
    echo "error: ninja not found. Run setup.sh first" >&2
    exit 1
fi

if ! command -v meson > /dev/null; then
    echo "error: meson not found. Run setup.sh first" >&2
    exit 1
fi

# Initialize submodules if needed
if [ ! -f "${PROJECT_ROOT}/third_party/mpv/meson.build" ]; then
    echo "Initializing git submodules..."
    (cd "${PROJECT_ROOT}" && git submodule update --init --recursive)
fi

# Download CEF if needed
if [ ! -d "${PROJECT_ROOT}/third_party/cef" ]; then
    echo "Downloading CEF..."
    python3 "${PROJECT_ROOT}/dev/download_cef.py"
fi

# Configure
echo "Configuring..."
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    "${PROJECT_ROOT}"

# Build
echo "Building..."
cmake --build "${BUILD_DIR}"

echo ""
echo "Build complete!"
echo "Executable: ${BUILD_DIR}/jellyfin-desktop-cef"
