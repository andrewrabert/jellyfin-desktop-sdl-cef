#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="$PROJECT_ROOT/AppImage-output"

mkdir -p "$OUTPUT_DIR"

echo "Building Docker image..."
docker build -t jellyfin-desktop-cef-appimage "$SCRIPT_DIR"

echo "Extracting built files..."
docker run --rm -v "$OUTPUT_DIR:/output" jellyfin-desktop-cef-appimage

echo "Creating AppDir structure..."
APPDIR="$OUTPUT_DIR/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons"

# Copy binary and libs
cp "$OUTPUT_DIR/jellyfin-desktop-cef/jellyfin-desktop-cef" "$APPDIR/usr/bin/"
cp -r "$OUTPUT_DIR/jellyfin-desktop-cef/libcef" "$APPDIR/usr/lib/"
cp -r "$OUTPUT_DIR/jellyfin-desktop-cef/libmpv" "$APPDIR/usr/lib/"

# Copy desktop file and icon
cp "$OUTPUT_DIR/jellyfin-desktop-cef.desktop" "$APPDIR/"
cp "$OUTPUT_DIR/jellyfin-desktop-cef.svg" "$APPDIR/" 2>/dev/null || \
    cp "$PROJECT_ROOT/resources/linux/jellyfin-desktop-cef.svg" "$APPDIR/"

# Create AppRun
cat > "$APPDIR/AppRun" << 'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib/libcef/lib:$HERE/usr/lib/libmpv/lib:$LD_LIBRARY_PATH"
exec "$HERE/usr/bin/jellyfin-desktop-cef" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# Get appimagetool
if [ ! -f "$OUTPUT_DIR/appimagetool" ]; then
    echo "Downloading appimagetool..."
    curl -Lo "$OUTPUT_DIR/appimagetool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$OUTPUT_DIR/appimagetool"
fi

# Build AppImage
echo "Building AppImage..."
ARCH=x86_64 "$OUTPUT_DIR/appimagetool" "$APPDIR" "$OUTPUT_DIR/Jellyfin-Desktop-CEF-x86_64.AppImage"

echo "Done: $OUTPUT_DIR/Jellyfin-Desktop-CEF-x86_64.AppImage"
