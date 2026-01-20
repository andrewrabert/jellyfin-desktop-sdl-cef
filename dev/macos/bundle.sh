#!/usr/bin/env sh
# Jellyfin Media Player - macOS bundling script
# Run build.sh first
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Check build exists
if [ ! -f "${BUILD_DIR}/jellyfin-desktop-cef" ]; then
    echo "error: Build not found. Run build.sh first" >&2
    exit 1
fi

# Determine version
if [ -f "${PROJECT_ROOT}/VERSION" ]; then
    VERSION="$(cat "${PROJECT_ROOT}/VERSION")"
else
    VERSION="$(cd "${PROJECT_ROOT}" && git describe --tags --always --dirty 2>/dev/null || echo "0.0.0")"
fi

ARCH="$(uname -m)"
OUTPUT_DIR="${BUILD_DIR}/output"

echo "Creating app bundle via cmake install..."

# Run cmake install to create the app bundle
# This handles: copying files, fixing library paths, code signing
cmake --install "${BUILD_DIR}" --prefix "${OUTPUT_DIR}"

APP_DIR="${OUTPUT_DIR}/${APP_NAME}"

if [ ! -d "${APP_DIR}" ]; then
    echo "error: App bundle not created" >&2
    exit 1
fi

# Create DMG
echo "Creating DMG..."
DMG_NAME="JellyfinMediaPlayer-${VERSION}-macos-${ARCH}.dmg"
rm -f "${BUILD_DIR}/${DMG_NAME}"

# create-dmg returns non-zero if icon positioning fails (no icon), ignore that
create-dmg \
    --volname "Jellyfin Media Player v${VERSION}" \
    --no-internet-enable \
    --window-size 500 300 \
    --icon-size 100 \
    --icon "Jellyfin Media Player.app" 125 150 \
    --app-drop-link 375 150 \
    "${BUILD_DIR}/${DMG_NAME}" "${APP_DIR}" || true

# Verify DMG was created
if [ ! -f "${BUILD_DIR}/${DMG_NAME}" ]; then
    echo "error: DMG creation failed" >&2
    exit 1
fi

echo ""
echo "Bundle complete!"
echo "App: ${APP_DIR}"
echo "DMG: ${BUILD_DIR}/${DMG_NAME}"
