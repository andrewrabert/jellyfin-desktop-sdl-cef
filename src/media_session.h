#pragma once

#include <string>
#include <functional>
#include <cstdint>
#include <memory>

struct MediaMetadata {
    std::string title;
    std::string artist;
    std::string album;
    int track_number = 0;
    int64_t duration_us = 0;
    std::string art_url;       // Jellyfin URL
    std::string art_data_uri;  // base64 data URI after fetch
};

enum class PlaybackState { Stopped, Playing, Paused };

class MediaSessionBackend {
public:
    virtual ~MediaSessionBackend() = default;
    virtual void setMetadata(const MediaMetadata& meta) = 0;
    virtual void setArtwork(const std::string& dataUri) = 0;  // Update artwork separately
    virtual void setPlaybackState(PlaybackState state) = 0;
    virtual void setPosition(int64_t position_us) = 0;
    virtual void setVolume(double volume) = 0;
    virtual void setCanGoNext(bool can) = 0;
    virtual void setCanGoPrevious(bool can) = 0;
    virtual void setRate(double rate) = 0;
    virtual void emitSeeked(int64_t /*position_us*/) {}  // Emit Seeked signal (MPRIS only)
    virtual void update() = 0;  // Called from event loop to process events
    virtual int getFd() = 0;    // File descriptor for poll, -1 if none
};

class MediaSession {
public:
    MediaSession();
    ~MediaSession();

    void setMetadata(const MediaMetadata& meta);
    void setArtwork(const std::string& dataUri);  // Update artwork separately (async fetch)
    void setPlaybackState(PlaybackState state);
    void setPosition(int64_t position_us);
    void setVolume(double volume);
    void setCanGoNext(bool can);
    void setCanGoPrevious(bool can);
    void setRate(double rate);
    void emitSeeked(int64_t position_us);  // Emit Seeked signal (for MPRIS)

    // Called from event loop
    void update();
    int getFd();

    // Control callbacks (set by main.cpp)
    std::function<void()> onPlay;
    std::function<void()> onPause;
    std::function<void()> onPlayPause;
    std::function<void()> onStop;
    std::function<void(int64_t)> onSeek;  // position in microseconds
    std::function<void()> onNext;
    std::function<void()> onPrevious;
    std::function<void()> onRaise;
    std::function<void(bool)> onSetFullscreen;
    std::function<void(double)> onSetRate;

    // Backend access (for platform-specific callbacks)
    MediaSessionBackend* backend() { return backend_.get(); }

private:
    std::unique_ptr<MediaSessionBackend> backend_;
    PlaybackState state_ = PlaybackState::Stopped;
};
