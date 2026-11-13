#pragma once

#include <memory>

namespace GameEngine {
namespace Platform {

class IWebView;

/// The entry points of the Linux web view module, which WebView_Linux.cpp loads at run time, so
/// libEngine.so never depends on WebKitGTK. Each entry point runs on the thread that pumps the host
/// window's events. An X11 window id is an unsigned long, the type of the Xlib Window.
struct WebViewGtkModule {
    /// A web view whose window is a child of the X11 window parentWindow, or nullptr when GTK cannot
    /// start on the X11 display.
    std::unique_ptr<IWebView> (*Create)(unsigned long parentWindow);

    /// Runs the GTK events that are ready, without waiting. Called once per host frame.
    void (*PumpEvents)();

    /// The host saw a mouse press in parentWindow outside every web view: a web view of that window
    /// that holds the keyboard focus gives it back to parentWindow.
    void (*ReturnFocus)(unsigned long parentWindow);
};

/// The name of the module's one exported symbol, GeWebViewGtkModule.
inline constexpr const char* kWebViewGtkModuleEntry = "GeWebViewGtkModule";

} // namespace Platform
} // namespace GameEngine

extern "C" const GameEngine::Platform::WebViewGtkModule* GeWebViewGtkModule();
