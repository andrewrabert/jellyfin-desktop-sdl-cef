# Jellyfin Desktop CEF Flatpak

## Build Bundle

```bash
./build.sh
```

Creates `jellyfin-desktop-cef.flatpak`.

## Install Bundle

```bash
flatpak install --user jellyfin-desktop-cef.flatpak
```

## Development

Build and install directly:
```bash
flatpak-builder --install --user --force-clean build-dir org.jellyfin.JellyfinDesktopCEF.yml
```

Test run without installing:
```bash
flatpak-builder --user --force-clean build-dir org.jellyfin.JellyfinDesktopCEF.yml
flatpak-builder --run build-dir org.jellyfin.JellyfinDesktopCEF.yml jellyfin-desktop-cef
```
