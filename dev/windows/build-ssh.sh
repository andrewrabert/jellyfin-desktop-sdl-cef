#!/usr/bin/env sh
set -eu

REMOTE_DIR='C:/jellyfin-desktop-cef'
LOCAL_OUT='dist'
ARCHIVE='/tmp/jellyfin-desktop-cef.tar'

show_help() {
    cat << EOF
Usage: $(basename "$0") <ssh-host>

Sync, build, and package jellyfin-desktop-cef on a Windows VM via SSH.

Arguments:
    ssh-host    SSH host
EOF
}

if [ $# -lt 1 ]; then
    show_help >&2
    exit 1
fi

case "$1" in
    -h|--help)
        show_help
        exit 0
        ;;
esac

REMOTE="$1"

# --- Sync ---
# Create git archive of HEAD plus any uncommitted changes
git archive --format=tar HEAD > "$ARCHIVE"

# Add uncommitted tracked changes to the archive
git diff --name-only HEAD | while read -r file; do
    [ -f "$file" ] && tar --update -f "$ARCHIVE" "$file"
done

# Add untracked files (excluding gitignored)
git ls-files --others --exclude-standard | while read -r file; do
    [ -f "$file" ] && tar --update -f "$ARCHIVE" "$file"
done

# Copy to VM
scp "$ARCHIVE" "$REMOTE:/tmp/jellyfin-desktop-cef.tar"

# Extract on VM (overwrite existing files, preserve .git)
ssh "$REMOTE" "cd $REMOTE_DIR && tar -xf /tmp/jellyfin-desktop-cef.tar"

# Reset git state to match extracted files
ssh "$REMOTE" "cd $REMOTE_DIR && git reset HEAD"

# Clean up local archive
rm -f "$ARCHIVE"
ssh "$REMOTE" "del /tmp/jellyfin-desktop-cef.tar" 2>/dev/null || true

echo "Synced to $REMOTE:$REMOTE_DIR"

# --- Build ---
ssh "$REMOTE" 'C:\jellyfin-desktop-cef\dev\windows\build.bat'

# --- Package ---
ssh "$REMOTE" "cd $REMOTE_DIR/build && cmake --install . --prefix install && cpack -G ZIP"

# Find and copy zip back
mkdir -p "$LOCAL_OUT"
scp "$REMOTE:$REMOTE_DIR/build/jellyfin-desktop-cef-*.zip" "$LOCAL_OUT/"

echo "Created:"
ls -lh "$LOCAL_OUT"/*.zip
