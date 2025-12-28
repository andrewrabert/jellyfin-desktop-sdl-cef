# mpv Video Player with CEF Overlay Design

## Overview

Add libmpv video playback with transparent CEF overlay that fades based on user activity.

## Usage

```bash
./jellyfin-desktop /path/to/video.mp4 --single-process
```

## Architecture

```
┌─────────────────────────────────────────────┐
│              SDL2 Window (1280x720)          │
├─────────────────────────────────────────────┤
│   CEF Overlay (transparent, fades)           │
│   mpv Video Layer (always visible)           │
└─────────────────────────────────────────────┘
```

## Render Pipeline

1. mpv renders video frame to FBO texture
2. Draw mpv texture as fullscreen quad (opaque)
3. CEF renders HTML with transparency to texture
4. Draw CEF texture with alpha blending
5. Modulate CEF alpha based on activity timer

## Activity-Based Fade

- Mouse movement or keyboard input resets 5-second timer
- Timer running: overlay opacity = 1.0
- Timer expired: animate opacity → 0.0 (fade out)
- New activity during fade: snap back to 1.0

## Key Components

### MpvPlayer class
- Initialize mpv with `vo=libmpv`
- Create render context for OpenGL
- Render to FBO texture
- Handle playback commands

### Overlay fade logic
- Track last activity timestamp
- Calculate opacity: `max(0, 1 - (elapsed - 5) / fade_duration)`
- Pass opacity to fragment shader uniform

### Renderer changes
- Two textures: mpv + CEF
- Shader with alpha uniform for CEF layer
- Enable GL blending for compositing

## CEF Transparency

Set in browser settings:
- `CefBrowserSettings.background_color` = 0 (transparent)
- HTML body: `background: transparent`
