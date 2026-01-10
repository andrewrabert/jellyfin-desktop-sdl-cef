#include "media_session.h"

#ifdef __linux__
// Forward declaration - implemented in media_session_mpris.cpp
std::unique_ptr<MediaSessionBackend> createMprisBackend(MediaSession* session);
#endif

MediaSession::MediaSession() {
#ifdef __linux__
    backend_ = createMprisBackend(this);
#endif
}

MediaSession::~MediaSession() = default;

void MediaSession::setMetadata(const MediaMetadata& meta) {
    if (backend_) backend_->setMetadata(meta);
}

void MediaSession::setArtwork(const std::string& dataUri) {
    if (backend_) backend_->setArtwork(dataUri);
}

void MediaSession::setPlaybackState(PlaybackState state) {
    state_ = state;
    if (backend_) backend_->setPlaybackState(state);
}

void MediaSession::setPosition(int64_t position_us) {
    if (backend_) backend_->setPosition(position_us);
}

void MediaSession::setVolume(double volume) {
    if (backend_) backend_->setVolume(volume);
}

void MediaSession::setCanGoNext(bool can) {
    if (backend_) backend_->setCanGoNext(can);
}

void MediaSession::setCanGoPrevious(bool can) {
    if (backend_) backend_->setCanGoPrevious(can);
}

void MediaSession::setRate(double rate) {
    if (backend_) backend_->setRate(rate);
}

void MediaSession::emitSeeked(int64_t position_us) {
    if (backend_) backend_->emitSeeked(position_us);
}

void MediaSession::update() {
    if (backend_) backend_->update();
}

int MediaSession::getFd() {
    return backend_ ? backend_->getFd() : -1;
}
