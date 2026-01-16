#include "cef/cef_app.h"
#include "cef/resource_handler.h"
#include "settings.h"
#include "embedded_js.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_frame.h"
#include "include/wrapper/cef_helpers.h"
#include <iostream>
#include <cstring>

// Legacy globals (unused)
Uint32 SDL_PLAYVIDEO_EVENT = 0;

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

void App::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
    registrar->AddCustomScheme("app",
        CEF_SCHEME_OPTION_STANDARD |
        CEF_SCHEME_OPTION_SECURE |
        CEF_SCHEME_OPTION_LOCAL |
        CEF_SCHEME_OPTION_CORS_ENABLED);
}

void App::OnContextInitialized() {
    std::cerr << "CEF context initialized" << std::endl;
    CefRegisterSchemeHandlerFactory("app", "", new EmbeddedSchemeHandlerFactory());
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
    std::cerr << "OnContextCreated: " << frame->GetURL().ToString() << std::endl;

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
    jmpNative->SetValue("playerSetSpeed", CefV8Value::CreateFunction("playerSetSpeed", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("saveServerUrl", CefV8Value::CreateFunction("saveServerUrl", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("setFullscreen", CefV8Value::CreateFunction("setFullscreen", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("loadServer", CefV8Value::CreateFunction("loadServer", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyMetadata", CefV8Value::CreateFunction("notifyMetadata", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyPosition", CefV8Value::CreateFunction("notifyPosition", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifySeek", CefV8Value::CreateFunction("notifySeek", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyPlaybackState", CefV8Value::CreateFunction("notifyPlaybackState", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyArtwork", CefV8Value::CreateFunction("notifyArtwork", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyQueueChange", CefV8Value::CreateFunction("notifyQueueChange", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    jmpNative->SetValue("notifyRateChange", CefV8Value::CreateFunction("notifyRateChange", handler), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("jmpNative", jmpNative, V8_PROPERTY_ATTRIBUTE_READONLY);

    // Inject the JavaScript shim that creates window.api, window.NativeShell, etc.
    std::string shim_str(embedded_js.at("native-shim.js"));

    // Replace placeholder with saved server URL
    std::string placeholder = "__SERVER_URL__";
    size_t pos = shim_str.find(placeholder);
    if (pos != std::string::npos) {
        shim_str.replace(pos, placeholder.length(), Settings::instance().serverUrl());
    }
    frame->ExecuteJavaScript(shim_str, frame->GetURL(), 0);

    // Inject the player plugins
    frame->ExecuteJavaScript(embedded_js.at("mpv-player-core.js"), frame->GetURL(), 0);
    frame->ExecuteJavaScript(embedded_js.at("mpv-video-player.js"), frame->GetURL(), 0);
    frame->ExecuteJavaScript(embedded_js.at("mpv-audio-player.js"), frame->GetURL(), 0);
    frame->ExecuteJavaScript(embedded_js.at("input-plugin.js"), frame->GetURL(), 0);
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

    // playerLoad(url, startMs, audioIdx, subIdx, metadataJson)
    if (name == "playerLoad") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string url = arguments[0]->GetStringValue().ToString();
            int startMs = arguments.size() > 1 && arguments[1]->IsInt() ? arguments[1]->GetIntValue() : 0;
            int audioIdx = arguments.size() > 2 && arguments[2]->IsInt() ? arguments[2]->GetIntValue() : -1;
            int subIdx = arguments.size() > 3 && arguments[3]->IsInt() ? arguments[3]->GetIntValue() : -1;
            std::string metadataJson = arguments.size() > 4 && arguments[4]->IsString() ? arguments[4]->GetStringValue().ToString() : "{}";

            std::cerr << "[V8] playerLoad: " << url << " startMs=" << startMs << std::endl;

            // Send IPC message to browser process
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerLoad");
            CefRefPtr<CefListValue> args = msg->GetArgumentList();
            args->SetString(0, url);
            args->SetInt(1, startMs);
            args->SetInt(2, audioIdx);
            args->SetInt(3, subIdx);
            args->SetString(4, metadataJson);
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

    if (name == "playerSetSpeed") {
        if (arguments.size() >= 1 && arguments[0]->IsInt()) {
            int rateX1000 = arguments[0]->GetIntValue();
            std::cerr << "[V8] playerSetSpeed: " << rateX1000 << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("playerSetSpeed");
            msg->GetArgumentList()->SetInt(0, rateX1000);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyMetadata") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string metadata = arguments[0]->GetStringValue().ToString();
            std::cerr << "[V8] notifyMetadata: " << metadata.substr(0, 100) << "..." << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyMetadata");
            msg->GetArgumentList()->SetString(0, metadata);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyPosition") {
        if (arguments.size() >= 1 && arguments[0]->IsInt()) {
            int posMs = arguments[0]->GetIntValue();
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyPosition");
            msg->GetArgumentList()->SetInt(0, posMs);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifySeek") {
        if (arguments.size() >= 1 && arguments[0]->IsInt()) {
            int posMs = arguments[0]->GetIntValue();
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifySeek");
            msg->GetArgumentList()->SetInt(0, posMs);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyPlaybackState") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string state = arguments[0]->GetStringValue().ToString();
            std::cerr << "[V8] notifyPlaybackState: " << state << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyPlaybackState");
            msg->GetArgumentList()->SetString(0, state);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyArtwork") {
        if (arguments.size() >= 1 && arguments[0]->IsString()) {
            std::string artworkUri = arguments[0]->GetStringValue().ToString();
            std::cerr << "[V8] notifyArtwork: " << artworkUri.substr(0, 50) << "..." << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyArtwork");
            msg->GetArgumentList()->SetString(0, artworkUri);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyQueueChange") {
        if (arguments.size() >= 2 && arguments[0]->IsBool() && arguments[1]->IsBool()) {
            bool canNext = arguments[0]->GetBoolValue();
            bool canPrev = arguments[1]->GetBoolValue();
            std::cerr << "[V8] notifyQueueChange: canNext=" << canNext << " canPrev=" << canPrev << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyQueueChange");
            msg->GetArgumentList()->SetBool(0, canNext);
            msg->GetArgumentList()->SetBool(1, canPrev);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
        return true;
    }

    if (name == "notifyRateChange") {
        if (arguments.size() >= 1 && arguments[0]->IsDouble()) {
            double rate = arguments[0]->GetDoubleValue();
            std::cerr << "[V8] notifyRateChange: " << rate << std::endl;
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("notifyRateChange");
            msg->GetArgumentList()->SetDouble(0, rate);
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
