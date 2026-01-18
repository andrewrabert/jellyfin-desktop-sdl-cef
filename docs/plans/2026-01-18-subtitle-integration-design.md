# Subtitle Integration Design

Hook up web subtitle button to mpv, matching old jellyfin-desktop behavior.

## Index Mapping

Jellyfin sends global `MediaStream.Index`. mpv expects 1-based type-relative `sid`. Convert in JS using `getRelativeIndexByType()` (same as old app).

| Jellyfin | mpv sid |
|----------|---------|
| Global index (e.g., 5) | Relative (e.g., 2) |
| `-1` | `"no"` |
| External w/ URL | `"#,URL"` |

## Changes Required

### 1. native-shim.js

Add `getRelativeIndexByType()` helper (copy from old app):
```javascript
getRelativeIndexByType(mediaStreams, jellyIndex, streamType) {
    let relIndex = 1;
    for (const source of mediaStreams) {
        if (source.Type != streamType || source.IsExternal) continue;
        if (source.Index == jellyIndex) return relIndex;
        relIndex += 1;
    }
    return null;
}
```

Store `mediaStreams` from `load()` call's `streamdata` parameter.

Implement `setSubtitleStream()`:
- Convert jellyIndex to relative via helper
- Handle external subs (DeliveryMethod=External → use URL)
- Call `window.jmpNative.playerSetSubtitle(relativeIndexOrUrl)`

### 2. cef_app.cpp - NativeV8Handler::Execute()

Add `playerSetSubtitle` function:
- Accept int (relative index) or string (external URL format)
- Send IPC message to browser process

### 3. cef_client.cpp - OnProcessMessageReceived()

Handle `playerSetSubtitle` message:
- Pass to `on_player_msg_("subtitle", ...)` callback

### 4. main.cpp - Command loop

Add `"subtitle"` command handling:
- Call `mpv.setSubtitleTrack()`

### 5. mpv_player_vk.h/.cpp

Add method:
```cpp
void setSubtitleTrack(int sid);  // -1 = off, 1+ = track
void setSubtitleTrack(const std::string& url);  // external
```

Implementation:
```cpp
void MpvPlayerVk::setSubtitleTrack(int sid) {
    if (sid < 0) {
        mpv_set_property_string(mpv_, "sid", "no");
    } else {
        int64_t id = sid;
        mpv_set_property(mpv_, "sid", MPV_FORMAT_INT64, &id);
    }
}
```

## Initial Play Subtitle

The `playerLoad` IPC already accepts `subIdx`. Need to:
1. In native-shim.js `load()`: convert jellyIndex to relative, pass to jmpNative
2. In main.cpp: use the subtitle param when calling mpv loadFile

## Audio Streams (Bonus)

Same pattern applies - `aid` property, `getRelativeIndexByType(..., 'Audio')`.

## Files to Modify

1. `src/web/native-shim.js` - index conversion, store mediaStreams
2. `src/cef/cef_app.cpp` - V8 handler for playerSetSubtitle
3. `src/cef/cef_client.cpp` - IPC message routing
4. `src/main.cpp` - command handling
5. `src/player/mpv/mpv_player_vk.h` - add setSubtitleTrack()
6. `src/player/mpv/mpv_player_vk.cpp` - implement setSubtitleTrack()
