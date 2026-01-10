#include "cef_app.h"
#include "settings.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_frame.h"
#include "include/wrapper/cef_helpers.h"
#include <iostream>
#include <cstring>
#include <sstream>
#include <fstream>

// Legacy globals (unused)
Uint32 SDL_PLAYVIDEO_EVENT = 0;

// Read file contents helper
static std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[Native] Failed to read: " << path << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void App::OnBeforeCommandLineProcessing(const CefString& process_type,
                                        CefRefPtr<CefCommandLine> command_line) {
    // Disable all Google services
    command_line->AppendSwitch("disable-background-networking");
    command_line->AppendSwitch("disable-client-side-phishing-detection");
    command_line->AppendSwitch("disable-default-apps");
    command_line->AppendSwitch("disable-extensions");
    command_line->AppendSwitch("disable-component-update");
    command_line->AppendSwitch("disable-sync");
    command_line->AppendSwitch("disable-translate");
    command_line->AppendSwitch("disable-domain-reliability");
    command_line->AppendSwitch("disable-breakpad");
    command_line->AppendSwitch("disable-notifications");
    command_line->AppendSwitch("disable-spell-checking");
    command_line->AppendSwitch("no-pings");
    command_line->AppendSwitch("bwsi");  // Browse without sign-in
    command_line->AppendSwitchWithValue("disable-features",
        "PushMessaging,BackgroundSync,SafeBrowsing,Translate,OptimizationHints,"
        "MediaRouter,DialMediaRouteProvider,AcceptCHFrame,AutofillServerCommunication,"
        "CertificateTransparencyComponentUpdater,SyncNotificationServiceWhenSignedIn,"
        "SpellCheck,SpellCheckService,PasswordManager");
    // Empty API keys prevent any Google API calls
    command_line->AppendSwitchWithValue("google-api-key", "");
    command_line->AppendSwitchWithValue("google-default-client-id", "");
    command_line->AppendSwitchWithValue("google-default-client-secret", "");

#ifdef __APPLE__
    // macOS: Use mock keychain to avoid system keychain prompts
    command_line->AppendSwitch("use-mock-keychain");
    // Disable software rasterizer to avoid GPU process issues
    command_line->AppendSwitch("disable-software-rasterizer");
#endif

    // Disable GPU rendering unless --gpu-overlay is specified
    // Software rendering is more stable and performs well for UI overlays
    if (!gpu_overlay_enabled_) {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
    }
}

void App::OnContextInitialized() {
    std::cerr << "CEF context initialized" << std::endl;
}

void App::OnScheduleMessagePumpWork(int64_t delay_ms) {
    // Called by CEF when it needs CefDoMessageLoopWork() to be called
    // delay_ms == 0: immediate work needed
    // delay_ms > 0: work needed after delay
    cef_work_delay_ms_.store(delay_ms);
    cef_work_pending_.store(true);
}

void App::OnContextCreated(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) {
    std::cerr << "[Native] OnContextCreated: " << frame->GetURL().ToString() << std::endl;

    // Load settings (renderer process is separate from browser process)
    Settings::instance().load();

    CefRefPtr<CefV8Value> window = context->GetGlobal();
    CefRefPtr<NativeV8Handler> handler = new NativeV8Handler(browser);

    // Create window.jmpNative for native calls
    CefRefPtr<CefV8Value> jmpNative = CefV8Value::CreateObject(nullptr, nullptr);
    jmpNative->SetValue("playerLoad", CefV8Value::CreateFunction("playerLoad", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerStop", CefV8Value::CreateFunction("playerStop", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerPause", CefV8Value::CreateFunction("playerPause", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerPlay", CefV8Value::CreateFunction("playerPlay", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSeek", CefV8Value::CreateFunction("playerSeek", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSetVolume", CefV8Value::CreateFunction("playerSetVolume", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("playerSetMuted", CefV8Value::CreateFunction("playerSetMuted", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("saveServerUrl", CefV8Value::CreateFunction("saveServerUrl", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("setFullscreen", CefV8Value::CreateFunction("setFullscreen", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("loadServer", CefV8Value::CreateFunction("loadServer", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("jmpNative", jmpNative, V8_PROPERTY_ATTRIBUTE_READONLY);

    // Inject the JavaScript shim that creates window.api, window.NativeShell, etc.
    const char* native_shim = R"JS(
(function() {
    console.log('[Native] Installing native shim...');

    // Fullscreen state tracking
    window._isFullscreen = false;

    // Signal emulation (Qt-style connect/disconnect)
    function createSignal(name) {
        const callbacks = [];
        const signal = function(...args) {
            console.log('[Signal] ' + name + ' firing with', callbacks.length, 'listeners');
            for (const cb of callbacks) {
                try { cb(...args); } catch(e) { console.error('[Signal] ' + name + ' error:', e); }
            }
        };
        signal.connect = (cb) => {
            callbacks.push(cb);
            console.log('[Signal] ' + name + ' connected, now has', callbacks.length, 'listeners');
        };
        signal.disconnect = (cb) => {
            const idx = callbacks.indexOf(cb);
            if (idx >= 0) callbacks.splice(idx, 1);
            console.log('[Signal] ' + name + ' disconnected, now has', callbacks.length, 'listeners');
        };
        return signal;
    }

    // window.jmpInfo - settings and device info
    window.jmpInfo = {
        version: '1.0.0',
        deviceName: 'Jellyfin Desktop CEF',
        mode: 'desktop',
        userAgent: navigator.userAgent,
        scriptPath: '',
        sections: [
            { key: 'main', order: 0 },
            { key: 'audio', order: 1 },
            { key: 'video', order: 2 }
        ],
        settings: {
            main: { enableMPV: true, fullscreen: false, userWebClient: '__SERVER_URL__' },
            audio: { channels: '2.0' },
            video: {
                force_transcode_dovi: false,
                force_transcode_hdr: false,
                force_transcode_hi10p: false,
                force_transcode_hevc: false,
                force_transcode_av1: false,
                force_transcode_4k: false,
                always_force_transcode: false,
                allow_transcode_to_hevc: true,
                prefer_transcode_to_h265: false,
                aspect: 'normal',
                default_playback_speed: 1
            }
        },
        settingsDescriptions: {
            video: [{ key: 'aspect', options: [
                { value: 'normal', title: 'video.aspect.normal' },
                { value: 'zoom', title: 'video.aspect.zoom' },
                { value: 'stretch', title: 'video.aspect.stretch' }
            ]}]
        },
        settingsUpdate: [],
        settingsDescriptionsUpdate: []
    };

    // Player state
    const playerState = {
        position: 0,
        duration: 0,
        volume: 100,
        muted: false,
        paused: false
    };

    // window.api.player - MPV control API
    window.api = {
        player: {
            // Signals (Qt-style)
            playing: createSignal('playing'),
            paused: createSignal('paused'),
            finished: createSignal('finished'),
            stopped: createSignal('stopped'),
            canceled: createSignal('canceled'),
            error: createSignal('error'),
            buffering: createSignal('buffering'),
            positionUpdate: createSignal('positionUpdate'),
            updateDuration: createSignal('updateDuration'),
            stateChanged: createSignal('stateChanged'),
            videoPlaybackActive: createSignal('videoPlaybackActive'),
            windowVisible: createSignal('windowVisible'),
            onVideoRecangleChanged: createSignal('onVideoRecangleChanged'),
            onMetaData: createSignal('onMetaData'),

            // Methods
            load(url, options, streamdata, audioStream, subtitleStream, callback) {
                console.log('[Native] player.load:', url);
                if (callback) {
                    // Wait for playing signal before calling callback
                    const onPlaying = () => {
                        this.playing.disconnect(onPlaying);
                        this.error.disconnect(onError);
                        callback();
                    };
                    const onError = () => {
                        this.playing.disconnect(onPlaying);
                        this.error.disconnect(onError);
                        callback();
                    };
                    this.playing.connect(onPlaying);
                    this.error.connect(onError);
                }
                if (window.jmpNative && window.jmpNative.playerLoad) {
                    window.jmpNative.playerLoad(url, options?.startMilliseconds || 0, audioStream || -1, subtitleStream || -1);
                }
            },
            stop() {
                console.log('[Native] player.stop');
                if (window.jmpNative) window.jmpNative.playerStop();
            },
            pause() {
                console.log('[Native] player.pause');
                if (window.jmpNative) window.jmpNative.playerPause();
                playerState.paused = true;
            },
            play() {
                console.log('[Native] player.play');
                if (window.jmpNative) window.jmpNative.playerPlay();
                playerState.paused = false;
            },
            seekTo(ms) {
                console.log('[Native] player.seekTo:', ms);
                if (window.jmpNative) window.jmpNative.playerSeek(ms);
            },
            setVolume(vol) {
                console.log('[Native] player.setVolume:', vol);
                playerState.volume = vol;
                if (window.jmpNative) window.jmpNative.playerSetVolume(vol);
            },
            setMuted(muted) {
                console.log('[Native] player.setMuted:', muted);
                playerState.muted = muted;
                if (window.jmpNative) window.jmpNative.playerSetMuted(muted);
            },
            setPlaybackRate(rate) {
                console.log('[Native] player.setPlaybackRate:', rate);
            },
            setSubtitleStream(stream) {
                console.log('[Native] player.setSubtitleStream:', stream);
            },
            setAudioStream(stream) {
                console.log('[Native] player.setAudioStream:', stream);
            },
            setSubtitleDelay(ms) {
                console.log('[Native] player.setSubtitleDelay:', ms);
            },
            setVideoRectangle(x, y, w, h) {
                // No-op for now, we always render fullscreen
            },
            getPosition(callback) {
                if (callback) callback(playerState.position);
                return playerState.position;
            },
            getDuration(callback) {
                if (callback) callback(playerState.duration);
                return playerState.duration;
            },
            notifyFullscreenChange(fs) {},
            notifyRateChange(rate) {}
        },
        system: {
            openExternalUrl(url) {
                window.open(url, '_blank');
            },
            exit() {
                console.log('[Native] exit requested');
            },
            cancelServerConnectivity() {
                if (window.jmpCheckServerConnectivity && window.jmpCheckServerConnectivity.abort) {
                    window.jmpCheckServerConnectivity.abort();
                }
            }
        },
        settings: {
            setValue(section, key, value, callback) {
                if (callback) callback();
            },
            sectionValueUpdate: createSignal('sectionValueUpdate'),
            groupUpdate: createSignal('groupUpdate')
        },
        input: {
            executeActions(actions) {
                console.log('[Native] executeActions:', actions);
                for (const action of actions) {
                    if (action === 'host:fullscreen') {
                        const newState = !window._isFullscreen;
                        window._isFullscreen = newState;
                        if (window.jmpNative) window.jmpNative.setFullscreen(newState);
                    }
                }
            }
        },
        window: {
            setCursorVisibility(visible) {}
        }
    };

    // Expose signal emitter for native code
    window._nativeEmit = function(signal, ...args) {
        console.log('[Native] _nativeEmit called with signal:', signal, 'args:', args);
        if (window.api && window.api.player && window.api.player[signal]) {
            console.log('[Native] Firing signal:', signal);
            window.api.player[signal](...args);
        } else {
            console.error('[Native] Signal not found:', signal, 'api exists:', !!window.api);
        }
    };
    window._nativeUpdatePosition = function(ms) {
        playerState.position = ms;
        window.api.player.positionUpdate(ms);
    };
    window._nativeUpdateDuration = function(ms) {
        playerState.duration = ms;
        window.api.player.updateDuration(ms);
    };

    // window.NativeShell - app info and plugins
    const plugins = ['mpvVideoPlayer', 'mpvAudioPlayer'];
    for (const plugin of plugins) {
        window[plugin] = () => window['_' + plugin];
    }

    window.NativeShell = {
        openUrl(url, target) {
            window.api.system.openExternalUrl(url);
        },
        downloadFile(info) {
            window.api.system.openExternalUrl(info.url);
        },
        openClientSettings() {},
        getPlugins() {
            return plugins;
        }
    };

    // Device profile for direct play
    function getDeviceProfile() {
        return {
            Name: 'Jellyfin Desktop',
            MaxStaticBitrate: 1000000000,
            MusicStreamingTranscodingBitrate: 1280000,
            TimelineOffsetSeconds: 5,
            TranscodingProfiles: [
                { Type: 'Audio' },
                {
                    Container: 'ts',
                    Type: 'Video',
                    Protocol: 'hls',
                    AudioCodec: 'aac,mp3,ac3,opus,vorbis',
                    VideoCodec: 'h264,h265,hevc,mpeg4,mpeg2video',
                    MaxAudioChannels: '6'
                },
                { Container: 'jpeg', Type: 'Photo' }
            ],
            DirectPlayProfiles: [
                { Type: 'Video' },
                { Type: 'Audio' },
                { Type: 'Photo' }
            ],
            ResponseProfiles: [],
            ContainerProfiles: [],
            CodecProfiles: [],
            SubtitleProfiles: [
                { Format: 'srt', Method: 'External' },
                { Format: 'srt', Method: 'Embed' },
                { Format: 'ass', Method: 'External' },
                { Format: 'ass', Method: 'Embed' },
                { Format: 'sub', Method: 'Embed' },
                { Format: 'ssa', Method: 'Embed' },
                { Format: 'pgssub', Method: 'Embed' },
                { Format: 'dvdsub', Method: 'Embed' }
            ]
        };
    }

    window.NativeShell.AppHost = {
        init() {
            return Promise.resolve({
                deviceName: jmpInfo.deviceName,
                appName: 'Jellyfin Desktop',
                appVersion: jmpInfo.version
            });
        },
        getDefaultLayout() {
            return jmpInfo.mode;
        },
        supports(command) {
            const features = [
                'filedownload', 'displaylanguage', 'htmlaudioautoplay',
                'htmlvideoautoplay', 'externallinks', 'multiserver',
                'fullscreenchange', 'remotevideo', 'displaymode'
            ];
            return features.includes(command.toLowerCase());
        },
        getDeviceProfile,
        getSyncProfile: getDeviceProfile,
        appName() { return 'Jellyfin Desktop'; },
        appVersion() { return jmpInfo.version; },
        deviceName() { return jmpInfo.deviceName; },
        exit() { window.api.system.exit(); }
    };

    window.initCompleted = Promise.resolve();
    window.apiPromise = Promise.resolve(window.api);

    console.log('[Native] Native shim installed');
})();
)JS";

    // Replace placeholder with saved server URL
    std::string shim_str(native_shim);
    std::string placeholder = "__SERVER_URL__";
    size_t pos = shim_str.find(placeholder);
    if (pos != std::string::npos) {
        shim_str.replace(pos, placeholder.length(), Settings::instance().serverUrl());
    }
    frame->ExecuteJavaScript(shim_str, frame->GetURL(), 0);

    // Now inject the mpvVideoPlayer plugin
    const char* mpv_video_player = R"JS(
(function() {
    function getMediaStreamAudioTracks(mediaSource) {
        return mediaSource.MediaStreams.filter(s => s.Type === 'Audio');
    }

    class mpvVideoPlayer {
        constructor({ events, loading, appRouter, globalize, appHost, appSettings, confirm, dashboard }) {
            this.events = events;
            this.loading = loading;
            this.appRouter = appRouter;
            this.globalize = globalize;
            this.appHost = appHost;
            this.appSettings = appSettings;
            if (dashboard && dashboard.default) {
                this.setTransparency = dashboard.default.setBackdropTransparency.bind(dashboard);
            } else {
                this.setTransparency = () => {};
            }

            this.name = 'MPV Video Player';
            this.type = 'mediaplayer';
            this.id = 'mpvvideoplayer';
            this.syncPlayWrapAs = 'htmlvideoplayer';
            this.priority = -1;
            this.useFullSubtitleUrls = true;
            this.isLocalPlayer = true;
            this.isFetching = false;

            this._videoDialog = undefined;
            this._currentSrc = undefined;
            this._started = false;
            this._timeUpdated = false;
            this._currentTime = 0;
            this._currentPlayOptions = undefined;
            this._duration = undefined;
            this._paused = false;
            this._volume = 100;
            this._muted = false;
            this._playRate = 1;
            this._hasConnection = false;

            this.onEnded = () => this.onEndedInternal();
            this.onTimeUpdate = (time) => {
                if (time && !this._timeUpdated) this._timeUpdated = true;
                this._currentTime = time;
                this.events.trigger(this, 'timeupdate');
            };
            this.onPlaying = () => {
                console.log('[MPV] onPlaying callback fired, _started:', this._started);
                if (!this._started) {
                    this._started = true;
                    console.log('[MPV] First play - hiding loading, showing OSD');
                    this.loading.hide();
                    const dlg = this._videoDialog;
                    if (dlg) dlg.style.backgroundImage = '';
                    if (this._currentPlayOptions?.fullscreen) {
                        this.appRouter.showVideoOsd();
                        if (dlg) dlg.style.zIndex = 'unset';
                    }
                    window.api.player.setVideoRectangle(0, 0, 0, 0);
                }
                if (this._paused) {
                    this._paused = false;
                    this.events.trigger(this, 'unpause');
                }
                this.events.trigger(this, 'playing');
                console.log('[MPV] playing event triggered');
            };
            this.onPause = () => {
                this._paused = true;
                this.events.trigger(this, 'pause');
            };
            this.onError = (error) => {
                this.removeMediaDialog();
                console.error('media error:', error);
                this.events.trigger(this, 'error', [{ type: 'mediadecodeerror' }]);
            };
            this.onDuration = (duration) => {
                this._duration = duration;
            };
        }

        currentSrc() { return this._currentSrc; }

        async play(options) {
            console.log('[MPV] play() called with options:', options);
            this._started = false;
            this._timeUpdated = false;
            this._currentTime = null;
            if (options.fullscreen) this.loading.show();
            await this.createMediaElement(options);
            console.log('[MPV] createMediaElement done, calling setCurrentSrc');
            return await this.setCurrentSrc(options);
        }

        getSavedVolume() {
            return this.appSettings.get('volume') || 1;
        }

        setCurrentSrc(options) {
            return new Promise((resolve) => {
                const val = options.url;
                this._currentSrc = val;
                console.log('[MPV] Playing:', val);

                const ms = (options.playerStartPositionTicks || 0) / 10000;
                this._currentPlayOptions = options;

                const audioIdx = options.mediaSource.DefaultAudioStreamIndex ?? -1;
                const subIdx = options.mediaSource.DefaultSubtitleStreamIndex ?? -1;

                window.api.player.load(val,
                    { startMilliseconds: ms, autoplay: true },
                    { type: 'video' },
                    audioIdx,
                    subIdx,
                    resolve);
            });
        }

        setSubtitleStreamIndex(index) {
            window.api.player.setSubtitleStream(index);
        }

        setSecondarySubtitleStreamIndex(index) {}

        resetSubtitleOffset() {
            window.api.player.setSubtitleDelay(0);
        }

        enableShowingSubtitleOffset() {}
        disableShowingSubtitleOffset() {}
        isShowingSubtitleOffsetEnabled() { return false; }
        setSubtitleOffset(offset) { window.api.player.setSubtitleDelay(Math.round(offset * 1000)); }
        getSubtitleOffset() { return 0; }

        setAudioStreamIndex(index) {
            window.api.player.setAudioStream(index);
        }

        onEndedInternal() {
            this.events.trigger(this, 'stopped', [{ src: this._currentSrc }]);
            this._currentTime = null;
            this._currentSrc = null;
            this._currentPlayOptions = null;
        }

        stop(destroyPlayer) {
            window.api.player.stop();
            this.onEndedInternal();
            if (destroyPlayer) this.destroy();
            return Promise.resolve();
        }

        removeMediaDialog() {
            window.api.player.stop();
            window.api.player.setVideoRectangle(-1, 0, 0, 0);
            document.body.classList.remove('hide-scroll');
            const dlg = this._videoDialog;
            if (dlg) {
                this.setTransparency(0);
                this._videoDialog = null;
                dlg.parentNode.removeChild(dlg);
            }
        }

        destroy() {
            this.removeMediaDialog();
            const player = window.api.player;
            this._hasConnection = false;
            player.playing.disconnect(this.onPlaying);
            player.positionUpdate.disconnect(this.onTimeUpdate);
            player.finished.disconnect(this.onEnded);
            player.updateDuration.disconnect(this.onDuration);
            player.error.disconnect(this.onError);
            player.paused.disconnect(this.onPause);
        }

        createMediaElement(options) {
            console.log('[MPV] createMediaElement called, _hasConnection:', this._hasConnection);
            let dlg = document.querySelector('.videoPlayerContainer');
            if (!dlg) {
                dlg = document.createElement('div');
                dlg.classList.add('videoPlayerContainer');
                dlg.style.cssText = 'position:fixed;top:0;bottom:0;left:0;right:0;display:flex;align-items:center;';
                if (options.fullscreen) dlg.style.zIndex = 1000;
                if (options.backdropUrl) {
                    dlg.style.backgroundImage = `url('${options.backdropUrl}')`;
                    dlg.style.backgroundSize = 'cover';
                    dlg.style.backgroundPosition = 'center';
                }
                document.body.insertBefore(dlg, document.body.firstChild);
                this.setTransparency(2);
                this._videoDialog = dlg;

                const player = window.api.player;
                if (!this._hasConnection) {
                    console.log('[MPV] Connecting signals to window.api.player');
                    this._hasConnection = true;
                    player.playing.connect(this.onPlaying);
                    player.positionUpdate.connect(this.onTimeUpdate);
                    player.finished.connect(this.onEnded);
                    player.updateDuration.connect(this.onDuration);
                    player.error.connect(this.onError);
                    player.paused.connect(this.onPause);
                    console.log('[MPV] Signals connected');
                }
            }
            if (options.fullscreen) document.body.classList.add('hide-scroll');
            return Promise.resolve();
        }

        canPlayMediaType(mediaType) {
            return (mediaType || '').toLowerCase() === 'video';
        }
        canPlayItem(item) { return this.canPlayMediaType(item.MediaType); }
        supportsPlayMethod() { return true; }
        getDeviceProfile(item, options) {
            return this.appHost.getDeviceProfile ? this.appHost.getDeviceProfile(item, options) : Promise.resolve({});
        }
        static getSupportedFeatures() { return ['PlaybackRate', 'SetAspectRatio']; }
        supports(feature) { return mpvVideoPlayer.getSupportedFeatures().includes(feature); }
        isFullscreen() { return window._isFullscreen === true; }
        toggleFullscreen() { window.api?.input?.executeActions(['host:fullscreen']); }

        currentTime(val) {
            if (val != null) { window.api.player.seekTo(val); return; }
            return this._currentTime;
        }
        currentTimeAsync() {
            return new Promise(resolve => window.api.player.getPosition(resolve));
        }
        duration() { return this._duration || null; }
        canSetAudioStreamIndex() { return true; }
        setPictureInPictureEnabled() {}
        isPictureInPictureEnabled() { return false; }
        isAirPlayEnabled() { return false; }
        setAirPlayEnabled() {}
        setBrightness() {}
        getBrightness() { return 100; }
        seekable() { return Boolean(this._duration); }
        pause() { window.api.player.pause(); }
        resume() { this._paused = false; window.api.player.play(); }
        unpause() { window.api.player.play(); }
        paused() { return this._paused; }

        setPlaybackRate(value) {
            this._playRate = +value;
            window.api.player.setPlaybackRate(this._playRate * 1000);
        }
        getPlaybackRate() { return this._playRate || 1; }
        getSupportedPlaybackRates() {
            return [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0].map(id => ({ name: id + 'x', id }));
        }

        saveVolume(value) { if (value) this.appSettings.set('volume', value); }
        setVolume(val, save = true) {
            val = Number(val);
            if (!isNaN(val)) {
                this._volume = val;
                if (save) { this.saveVolume(val / 100); this.events.trigger(this, 'volumechange'); }
                window.api.player.setVolume(val);
            }
        }
        getVolume() { return this._volume; }
        volumeUp() { this.setVolume(Math.min(this.getVolume() + 2, 100)); }
        volumeDown() { this.setVolume(Math.max(this.getVolume() - 2, 0)); }
        setMute(mute, triggerEvent = true) {
            this._muted = mute;
            window.api.player.setMuted(mute);
            if (triggerEvent) this.events.trigger(this, 'volumechange');
        }
        isMuted() { return this._muted; }
        togglePictureInPicture() {}
        toggleAirPlay() {}
        getBufferedRanges() { return []; }
        getStats() { return Promise.resolve({ categories: [] }); }
        getSupportedAspectRatios() { return []; }
        getAspectRatio() { return 'normal'; }
        setAspectRatio(value) {}
    }

    window._mpvVideoPlayer = mpvVideoPlayer;
    console.log('[Native] mpvVideoPlayer class installed, window.mpvVideoPlayer:', typeof window.mpvVideoPlayer);
    console.log('[Native] window._mpvVideoPlayer:', typeof window._mpvVideoPlayer);
})();
)JS";

    frame->ExecuteJavaScript(mpv_video_player, frame->GetURL(), 0);

    // Also inject a simple audio player stub
    const char* mpv_audio_player = R"JS(
(function() {
    class mpvAudioPlayer {
        constructor(opts) {
            this.name = 'MPV Audio Player';
            this.type = 'mediaplayer';
            this.id = 'mpvaudioplayer';
            this.priority = -1;
            this.isLocalPlayer = true;
        }
        canPlayMediaType(mediaType) {
            return (mediaType || '').toLowerCase() === 'audio';
        }
        canPlayItem(item) { return this.canPlayMediaType(item.MediaType); }
    }
    window._mpvAudioPlayer = mpvAudioPlayer;
    console.log('[Native] mpvAudioPlayer plugin installed');
})();
)JS";

    frame->ExecuteJavaScript(mpv_audio_player, frame->GetURL(), 0);
}

bool App::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefProcessId source_process,
                                   CefRefPtr<CefProcessMessage> message) {
    return false;
}

// V8 handler implementation - sends IPC messages to browser process
bool NativeV8Handler::Execute(const CefString& name,
                              CefRefPtr<CefV8Value> object,
                              const CefV8ValueList& arguments,
                              CefRefPtr<CefV8Value>& retval,
                              CefString& exception) {
    std::cerr << "[V8] Execute: " << name.ToString() << std::endl;

    // playerLoad(url, startMs, audioIdx, subIdx)
    if (name == "playerLoad") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string url = arguments[0]->GetStringValue().ToString();
            int64_t startMs = arguments.size() > 1 && arguments[1]->IsInt() ? arguments[1]->GetIntValue() : 0;

            std::cerr << "[V8] playerLoad: " << url << " startMs=" << startMs << std::endl;

            // Send IPC message to browser process
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerLoad");
            CefRefPtr<CefListValue> args = msg->GetArgumentList();
            args->SetString(0, url);
            args->SetInt(1, static_cast<int>(startMs));
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerStop") {
        std::cerr << "[V8] playerStop" << std::endl;
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerStop");
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (name == "playerPause") {
        std::cerr << "[V8] playerPause" << std::endl;
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerPause");
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (name == "playerPlay") {
        std::cerr << "[V8] playerPlay (unpause)" << std::endl;
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerPlay");
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (name == "playerSeek") {
        if (arguments.size() >= 1 && arguments[0]->IsInt()) {
            int64_t ms = arguments[0]->GetIntValue();
            std::cerr << "[V8] playerSeek: " << ms << "ms" << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSeek");
            msg->GetArgumentList()->SetInt(0, static_cast<int>(ms));
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerSetVolume") {
        if (arguments.size() >= 1 && arguments[0]->IsInt()) {
            int vol = arguments[0]->GetIntValue();
            std::cerr << "[V8] playerSetVolume: " << vol << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetVolume");
            msg->GetArgumentList()->SetInt(0, vol);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "playerSetMuted") {
        if (arguments.size() >= 1 && arguments[0]->IsBool()) {
            bool muted = arguments[0]->GetBoolValue();
            std::cerr << "[V8] playerSetMuted: " << muted << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetMuted");
            msg->GetArgumentList()->SetBool(0, muted);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "saveServerUrl") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string url = arguments[0]->GetStringValue().ToString();
            std::cerr << "[V8] saveServerUrl: " << url << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("saveServerUrl");
            msg->GetArgumentList()->SetString(0, url);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "setFullscreen") {
        bool enable = arguments.size() >= 1 && arguments[0]->IsBool() ? arguments[0]->GetBoolValue() : true;
        std::cerr << "[V8] setFullscreen: " << enable << std::endl;
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("setFullscreen");
        msg->GetArgumentList()->SetBool(0, enable);
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    if (name == "loadServer") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string url = arguments[0]->GetStringValue().ToString();
            std::cerr << "[V8] loadServer: " << url << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("loadServer");
            msg->GetArgumentList()->SetString(0, url);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    return false;
}
