#!/usr/bin/env sh
# Jellyfin Media Player - Run built app
# Run build.sh first
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

EXECUTABLE="${BUILD_DIR}/jellyfin-desktop-cef"

# Check build exists
if [ ! -f "${EXECUTABLE}" ]; then
    echo "error: Build not found. Run build.sh first" >&2
    exit 1
fi

exec "${EXECUTABLE}" "${@}"
