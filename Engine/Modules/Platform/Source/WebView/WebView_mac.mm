#if defined(__APPLE__)

#include "Platform/WebView.h"
#include "Platform/Window.h"
#include "Logger/Logger.h"
#include "../MacWindowInput.h"
#include "WebViewPageStyle_mac.h"

#include <cstdlib>
#include <vector>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// Private API to enable HTML5 fullscreen in WKWebView (YouTube and other sites).
// Without this, pages show "Your browser doesn't support fullscreen".
@interface WKPreferences (Fullscreen)
- (void)_setFullScreenEnabled:(BOOL)enabled;
@end

// Custom navigation delegate to intercept YouTube embed requests and inject proper headers
@interface YouTubeNavigationDelegate : NSObject <WKNavigationDelegate, WKDownloadDelegate>
@property(nonatomic, strong) NSMapTable<WKDownload*, NSArray<NSURL*>*>* destinations;
@end

@interface BundledWebAppHandler : NSObject <WKURLSchemeHandler, WKScriptMessageHandlerWithReply, WKUIDelegate>
@property(nonatomic, strong) NSURL* directory;
@end

@implementation BundledWebAppHandler

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task
{
    (void)webView;
    NSURL* request = task.request.URL;
    NSURL* directory = self.directory;
    if (!directory || ![request.host isEqualToString:@"editor"] ||
        ![task.request.HTTPMethod isEqualToString:@"GET"])
    {
        [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorFileDoesNotExist userInfo:nil]];
        return;
    }

    NSString* path = request.path;
    NSString* relative = path.length > 1 ? [path substringFromIndex:1] : @"index.html";
    NSURL* file = [[directory URLByAppendingPathComponent:relative] URLByResolvingSymlinksInPath];
    NSString* prefix = [directory.path stringByAppendingString:@"/"];
    if (![file.path hasPrefix:prefix])
    {
        [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorNoPermissionsToReadFile userInfo:nil]];
        return;
    }

    NSData* bytes = [NSData dataWithContentsOfURL:file options:NSDataReadingMappedIfSafe error:nullptr];
    if (!bytes)
    {
        [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorFileDoesNotExist userInfo:nil]];
        return;
    }
    NSString* extension = file.pathExtension.lowercaseString;
    UTType* type = [UTType typeWithFilenameExtension:extension];
    NSString* mime = type.preferredMIMEType;
    if (!mime)
        mime = @"application/octet-stream";
    if ([extension isEqualToString:@"wasm"])
        mime = @"application/wasm";
    else if ([extension isEqualToString:@"js"] || [extension isEqualToString:@"mjs"])
        mime = @"text/javascript";
    NSHTTPURLResponse* response = [[NSHTTPURLResponse alloc] initWithURL:request
        statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:@{
            @"Content-Type": mime,
            @"Content-Length": [@(bytes.length) stringValue],
            @"Cross-Origin-Opener-Policy": @"same-origin",
            @"Cross-Origin-Embedder-Policy": @"require-corp"
        }];
    [task didReceiveResponse:response];
    [task didReceiveData:bytes];
    [task didFinish];
}

- (void)webView:(WKWebView*)webView runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
    initiatedByFrame:(WKFrameInfo*)frame completionHandler:(void (^)(NSArray<NSURL*>*))completion
{
    NSURL* source = frame.request.URL;
    if (!self.directory || !frame.isMainFrame ||
        ![source.scheme isEqualToString:@"lucia-app"] || ![source.host isEqualToString:@"editor"])
    {
        completion(nil);
        return;
    }

    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = parameters.allowsDirectories;
    panel.allowsMultipleSelection = parameters.allowsMultipleSelection;
    void (^finish)(NSModalResponse) = ^(NSModalResponse result) {
        completion(result == NSModalResponseOK ? panel.URLs : nil);
    };
    if (NSWindow* window = webView.window)
        [panel beginSheetModalForWindow:window completionHandler:finish];
    else
        [panel beginWithCompletionHandler:finish];
}

- (void)userContentController:(WKUserContentController*)controller
    didReceiveScriptMessage:(WKScriptMessage*)message replyHandler:(void (^)(id, NSString*))reply
{
    (void)controller;
    NSURL* source = message.frameInfo.request.URL;
    if (!self.directory || !message.frameInfo.isMainFrame ||
        ![source.scheme isEqualToString:@"lucia-app"] || ![source.host isEqualToString:@"editor"])
    {
        reply(nil, @"Only the bundled workspace can save documents.");
        return;
    }

    if ([message.name isEqualToString:@"luciaChooseDocumentClose"])
    {
        NSString* filename = [message.body isKindOfClass:NSString.class] ? message.body : @"Untitled";
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = [NSString stringWithFormat:@"Save OpenPencil changes to “%@”?", filename];
        alert.informativeText = @"Your changes will be lost if you don’t save them.";
        [alert addButtonWithTitle:@"Save"];
        [alert addButtonWithTitle:@"Cancel"];
        [alert addButtonWithTitle:@"Don’t Save"];
        [alert.buttons[1] setKeyEquivalent:@"\033"];
        void (^finish)(NSModalResponse) = ^(NSModalResponse response) {
            switch (response)
            {
            case NSAlertFirstButtonReturn: reply(@"save", nil); break;
            case NSAlertThirdButtonReturn: reply(@"discard", nil); break;
            default: reply(@"cancel", nil); break;
            }
        };
        if (NSWindow* window = message.webView.window)
            [alert beginSheetModalForWindow:window completionHandler:finish];
        else
            finish([alert runModal]);
        return;
    }

    NSDictionary* payload = [message.body isKindOfClass:NSDictionary.class] ? message.body : nil;
    NSString* filename = payload[@"filename"];
    NSString* contents = payload[@"contents"];
    if (![filename isKindOfClass:NSString.class] || ![contents isKindOfClass:NSString.class])
    {
        reply(nil, @"Invalid document data.");
        return;
    }

    NSData* bytes = [[NSData alloc] initWithBase64EncodedString:contents options:0];
    if (!bytes.length)
    {
        reply(nil, @"Could not read the document data.");
        return;
    }

    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = filename.lastPathComponent;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
        if (result != NSModalResponseOK || !panel.URL)
        {
            reply(NSNull.null, nil);
            return;
        }

        NSError* error = nil;
        if (![bytes writeToURL:panel.URL options:NSDataWritingAtomic error:&error])
        {
            reply(nil, error.localizedDescription);
            return;
        }
        reply(panel.URL.lastPathComponent, nil);
    }];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task
{
    (void)webView;
    (void)task;
}

@end

@interface WebViewFileDragSource : NSObject <NSDraggingSource>
@end


@implementation YouTubeNavigationDelegate

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    if ([webView.URL.scheme isEqualToString:@"lucia-app"] && navigationAction.shouldPerformDownload)
    {
        decisionHandler(WKNavigationActionPolicyDownload);
        return;
    }
    decisionHandler(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView*)webView navigationAction:(WKNavigationAction*)action
    didBecomeDownload:(WKDownload*)download
{
    (void)webView;
    (void)action;
    download.delegate = self;
}

- (void)download:(WKDownload*)download decideDestinationUsingResponse:(NSURLResponse*)response
    suggestedFilename:(NSString*)filename completionHandler:(void (^)(NSURL*))completionHandler
{
    (void)response;
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = filename.lastPathComponent;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
        if (result != NSModalResponseOK || !panel.URL)
        {
            completionHandler(nil);
            return;
        }

        // WKDownload requires a new file. Write next to the selected file and replace it
        // only when WebKit has finished and the save panel approved the destination.
        NSURL* directory = [[panel.URL URLByDeletingLastPathComponent]
            URLByAppendingPathComponent:[NSString stringWithFormat:@".lucia-download-%@", NSUUID.UUID.UUIDString]
            isDirectory:YES];
        NSError* error = nil;
        if (![NSFileManager.defaultManager createDirectoryAtURL:directory withIntermediateDirectories:NO
                                                   attributes:nil error:&error])
        {
            NSLog(@"Could not create a temporary web app download: %@", error);
            completionHandler(nil);
            return;
        }
        NSURL* temporary = [directory URLByAppendingPathComponent:@"download"];
        if (!self.destinations)
            self.destinations = [NSMapTable strongToStrongObjectsMapTable];
        [self.destinations setObject:@[temporary, panel.URL] forKey:download];
        completionHandler(temporary);
    }];
}

- (void)downloadDidFinish:(WKDownload*)download
{
    NSArray<NSURL*>* paths = [self.destinations objectForKey:download];
    [self.destinations removeObjectForKey:download];
    if (!paths)
        return;

    NSURL* temporary = paths[0];
    NSURL* destination = paths[1];
    NSFileManager* files = NSFileManager.defaultManager;
    NSError* error = nil;
    if ([files fileExistsAtPath:destination.path])
        [files replaceItemAtURL:destination withItemAtURL:temporary backupItemName:nil options:0
             resultingItemURL:nullptr error:&error];
    else
        [files moveItemAtURL:temporary toURL:destination error:&error];
    if (error)
    {
        NSAlert* alert = [NSAlert alertWithError:error];
        [alert runModal];
    }
    [files removeItemAtURL:[temporary URLByDeletingLastPathComponent] error:nullptr];
}

- (void)download:(WKDownload*)download didFailWithError:(NSError*)error resumeData:(NSData*)resumeData
{
    (void)resumeData;
    NSURL* temporary = [[self.destinations objectForKey:download] firstObject];
    [self.destinations removeObjectForKey:download];
    if (temporary)
        [NSFileManager.defaultManager removeItemAtURL:[temporary URLByDeletingLastPathComponent] error:nullptr];
    NSLog(@"Web app download failed: %@", error);
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    NSView* view = (NSView*)webView;
    if (view && view.layer) {
        view.layer.backgroundColor = [[NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0] CGColor];
    }
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    NSView* view = (NSView*)webView;
    if (view && view.layer) {
        view.layer.backgroundColor = [[NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0] CGColor];
    }
    
    // Inject JavaScript to ensure dark background persists and check for YouTube errors
    // Re-assert after navigation: some pages replace <head> during load.
    NSString* script = [GameEngine::Platform::Detail::kPageStyleScript stringByAppendingString:@""
        "(function() {"
        "  var iframes = document.querySelectorAll('iframe');"
        "  for (var i = 0; i < iframes.length; i++) {"
        "    iframes[i].style.backgroundColor = '#272727';"
        "  }"
        "})();"];
    
    [webView evaluateJavaScript:script completionHandler:^(id result, NSError* error) {
        if (error) {
            NSLog(@"JavaScript injection error: %@", error);
        }
        // Webview visibility is managed by the panel, don't force show here
    }];
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    NSLog(@"Navigation failed: %@", error);
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    NSLog(@"Provisional navigation failed: %@", error);
}

@end

@implementation WebViewFileDragSource

- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    (void)session;
    (void)context;
    return NSDragOperationCopy;
}

- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession*)session
{
    (void)session;
    return YES;
}

@end

namespace GameEngine {
namespace Platform {

class WebView_mac : public IWebView {
public:
    WebView_mac(Window* parentWindow) : m_ParentWindow(parentWindow) {
        if (!parentWindow) {
            return;
        }

        GLFWwindow* glfwWindow = parentWindow->GetGLFWHandle();
        if (!glfwWindow) {
            return;
        }

        // Get NSWindow from GLFW window
        NSWindow* window = glfwGetCocoaWindow(glfwWindow);
        if (!window) {
            return;
        }
        NSView* contentView = [window contentView];
        if (!contentView) {
            return;
        }
        m_HostView = contentView;

        // Create WKWebView with configuration for YouTube embeds
        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        m_BundledApp = [[BundledWebAppHandler alloc] init];
        [config setURLSchemeHandler:m_BundledApp forURLScheme:@"lucia-app"];
        // Keep the previous page visible until the next document is complete;
        // painting a partial document can expose empty content and changing scrollbars.
        config.suppressesIncrementalRendering = YES;
        config.allowsAirPlayForMediaPlayback = YES;
        config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
        
        // Enable JavaScript (required for YouTube embeds)
        WKPreferences* preferences = [[WKPreferences alloc] init];
        config.defaultWebpagePreferences.allowsContentJavaScript = YES;
        // Enable HTML5 fullscreen API so YouTube (and other sites) can go fullscreen
        if ([preferences respondsToSelector:@selector(_setFullScreenEnabled:)]) {
            [preferences _setFullScreenEnabled:YES];
        }
        config.preferences = preferences;
        
        // Configure website data store for proper cookie/security settings
        // Use default data store which supports cookies and secure connections
        config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
        
        // Set application name for user agent to appear as Safari
        if (@available(macOS 10.13, *)) {
            config.applicationNameForUserAgent = @"Version/17.0 Safari/605.1.15";
        }
        
        // Enable additional features for better compatibility
        if (@available(macOS 11.3, *)) {
            // Allow media playback without user gesture
            config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
        }
        
        // Create user content controller to inject referrer policy
        WKUserContentController* userContentController = [[WKUserContentController alloc] init];
        
        // Inject referrer policy and dark background script at document start
        NSString* injectScript = [GameEngine::Platform::Detail::kPageStyleScript stringByAppendingString:@""
            "(function() {"
            "  if (document.querySelector('meta[name=\"referrer\"]') !== null) {"
            "    return;"
            "  }"
            "  var meta = document.createElement('meta');"
            "  meta.name = 'referrer';"
            "  meta.content = 'strict-origin-when-cross-origin';"
            "  var attachMeta = function() {"
            "    if (document.head) {"
            "      document.head.appendChild(meta);"
            "    }"
            "  };"
            "  if (document.head) {"
            "    attachMeta();"
            "  } else {"
            "    document.addEventListener('DOMContentLoaded', attachMeta);"
            "  }"
            "})();"];
        
        WKUserScript* userScript = [[WKUserScript alloc] initWithSource:injectScript
                                                           injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                        forMainFrameOnly:NO]; // Inject in all frames
        [userContentController addUserScript:userScript];
        [userContentController addScriptMessageHandlerWithReply:m_BundledApp
            contentWorld:WKContentWorld.pageWorld name:@"luciaSaveDocument"];
        [userContentController addScriptMessageHandlerWithReply:m_BundledApp
            contentWorld:WKContentWorld.pageWorld name:@"luciaChooseDocumentClose"];
        config.userContentController = userContentController;

        m_WebView = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:config];
        
        // Enable layer-backed view for proper background color rendering
        NSView* webViewAsView = (NSView*)m_WebView;
        webViewAsView.wantsLayer = YES;
        if (webViewAsView.layer) {
            webViewAsView.layer.backgroundColor = [[NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0] CGColor];
        }
        
        // Set user agent to match latest Safari to avoid "browser not secure" errors
        // Use a modern Safari user agent string that YouTube recognizes as secure
        NSString* safariUserAgent = @"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15";
        [m_WebView setCustomUserAgent:safariUserAgent];
        
        // Configure for better YouTube compatibility and security
        [m_WebView setAllowsLinkPreview:NO];
        [m_WebView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [m_WebView setHidden:YES]; // Start hidden until positioned and content loaded
        [m_WebView setAllowsBackForwardNavigationGestures:NO];
        
        // Make the web content area non-opaque so the dark layer background
        // shows through until the page actually renders (prevents white flash).
        [m_WebView setValue:@NO forKey:@"drawsBackground"];

        if (@available(macOS 12.0, *)) {
            NSColor* underPage = [NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0];
            [m_WebView setUnderPageBackgroundColor:underPage];
        }
        
        // Enable media playback features
        if (@available(macOS 10.12.2, *)) {
            [m_WebView setAllowsMagnification:NO];
        }
        
        // Set navigation delegate to intercept YouTube embed requests
        m_NavigationDelegate = [[YouTubeNavigationDelegate alloc] init];
        [m_WebView setNavigationDelegate:m_NavigationDelegate];

        // Add to content view. Clip to bounds so the web view never draws outside the YouTube panel area.
        [contentView addSubview:m_WebView];
        [webViewAsView setClipsToBounds:YES];

        // WKWebView receives native input before the editor's input router.
        const NSEventMask mouseEvents = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown |
            NSEventMaskOtherMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp | NSEventMaskScrollWheel;
        m_MouseMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mouseEvents
            handler:^NSEvent*(NSEvent* event) {
                ObserveMouseEvent(event);
                return event;
            }];
        m_WindowResignObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidResignKeyNotification object:window queue:nil
            usingBlock:^(__unused NSNotification* notification) {
                EndMouseDrag();
            }];

        m_Initialized = true;
    }

    ~WebView_mac() override {
        Destroy();
    }

    bool LoadURL(const std::string& url) override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }

        EndMouseDrag();
        NSView* webViewAsView = (NSView*)m_WebView;
        if (webViewAsView && webViewAsView.layer) {
            webViewAsView.layer.backgroundColor = [[NSColor colorWithRed:39.0/255.0 green:39.0/255.0 blue:39.0/255.0 alpha:1.0] CGColor];
        }

        NSString* nsURL = [NSString stringWithUTF8String:url.c_str()];
        NSURL* requestURL = [NSURL URLWithString:nsURL];
        if (!requestURL) {
            return false;
        }

        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:requestURL];

        // Only set YouTube-specific Referer/Origin for YouTube URLs (required to prevent error 153/152-4).
        // Other sites may reject or misbehave if given a YouTube referer.
        if (url.find("youtube.com") != std::string::npos || url.find("youtu.be") != std::string::npos) {
            [request setValue:@"https://www.youtube.com" forHTTPHeaderField:@"Referer"];
            [request setValue:@"https://www.youtube.com" forHTTPHeaderField:@"Origin"];
        }
        if (@available(macOS 13.0, *)) {
            [request setValue:@"strict-origin-when-cross-origin" forHTTPHeaderField:@"Referrer-Policy"];
        }
        [request setCachePolicy:NSURLRequestReloadIgnoringLocalCacheData];
        [request setTimeoutInterval:30.0];

        [m_WebView loadRequest:request];
        return true;
    }

    bool LoadBundledApp(const std::filesystem::path& directory) override
    {
        if (!m_Initialized || !std::filesystem::is_regular_file(directory / "index.html"))
            return false;

        NSString* path = [NSString stringWithUTF8String:directory.c_str()];
        if (!path)
            return false;
        m_BundledApp.directory = [[NSURL fileURLWithPath:path isDirectory:YES] URLByResolvingSymlinksInPath];
        m_WebView.UIDelegate = m_BundledApp;
        return LoadURL("lucia-app://editor/");
    }

    bool LoadHTML(const std::string& html, const std::string& baseURL) override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }

        NSString* htmlString = [NSString stringWithUTF8String:html.c_str()];
        NSString* baseURLString = [NSString stringWithUTF8String:baseURL.c_str()];
        NSURL* baseNSURL = [NSURL URLWithString:baseURLString];
        
        if (!baseNSURL) {
            baseNSURL = [NSURL URLWithString:@"https://www.youtube.com"];
        }
        
        [m_WebView loadHTMLString:htmlString baseURL:baseNSURL];
        return true;
    }

    void GoBack() override {
        if (m_Initialized && m_WebView) {
            [m_WebView goBack];
        }
    }

    void GoForward() override {
        if (m_Initialized && m_WebView) {
            [m_WebView goForward];
        }
    }

    void Reload() override {
        if (m_Initialized && m_WebView) {
            [m_WebView reload];
        }
    }

    void StopLoading() override {
        if (m_Initialized && m_WebView) {
            [m_WebView stopLoading];
        }
    }

    WebViewNavigation GetNavigation() const override {
        if (!m_Initialized || !m_WebView) {
            return {};
        }
        NSString* url = m_WebView.URL.absoluteString;
        return {m_WebView.canGoBack == YES, m_WebView.canGoForward == YES, m_WebView.isLoading == YES,
                url ? std::string([url UTF8String]) : std::string()};
    }

    void* GetNativeView() const override {
        return (__bridge void*)m_WebView;
    }

    void Resize(int width, int height) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }

        const NSSize size = NSMakeSize(width, height);
        if (!NSEqualSizes(m_WebView.frame.size, size))
            [m_WebView setFrameSize:size];
    }

    void SetPosition(int x, int y) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }

        NSRect frame = [m_WebView frame];
        // Convert from top-left to bottom-left coordinate system
        GLFWwindow* glfwWindow = m_ParentWindow->GetGLFWHandle();
        if (glfwWindow) {
            NSWindow* window = glfwGetCocoaWindow(glfwWindow);
            if (window) {
                NSView* contentView = [window contentView];
                if (contentView) {
                    NSRect contentRect = [contentView bounds];
                    const NSPoint origin = NSMakePoint(x, contentRect.size.height - y - frame.size.height);
                    if (!NSEqualPoints(frame.origin, origin))
                        [m_WebView setFrameOrigin:origin];
                }
            }
        }
    }

    void SetVisible(bool visible) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }
        if (!visible)
            EndMouseDrag();
        if (m_WebView.hidden == visible)
            [m_WebView setHidden:!visible];
    }

    bool IsVisible() const override {
        if (!m_Initialized || !m_WebView) {
            return false;
        }
        return ![m_WebView isHidden];
    }

    bool HasNativeDragFramePriority() const override
    {
        if (!m_WebView || m_WebView.hidden || !m_WebView.window.isKeyWindow)
            return false;
        return (m_PointerDragActive && (NSEvent.pressedMouseButtons & 1u) != 0) ||
            m_ReleaseFramePriority.IsActive(CACurrentMediaTime());
    }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override {
        if (!m_Initialized || !m_WebView) {
            return;
        }
        // Set WKWebView background color on the underlying NSView's layer
        CGFloat red = static_cast<CGFloat>(r) / 255.0;
        CGFloat green = static_cast<CGFloat>(g) / 255.0;
        CGFloat blue = static_cast<CGFloat>(b) / 255.0;
        NSColor* bgColor = [NSColor colorWithRed:red green:green blue:blue alpha:1.0];
        
        NSView* view = (NSView*)m_WebView;
        if (view && view.layer) {
            view.layer.backgroundColor = [bgColor CGColor];
        }
        
        // Also try setting it via KVC (some versions of WKWebView support this)
        @try {
            [m_WebView setValue:bgColor forKey:@"backgroundColor"];
        } @catch (NSException* exception) {
            // Ignore if property doesn't exist
        }
    }

    bool BeginFileDrag(const std::filesystem::path& path) override {
        if (!m_Initialized || !m_WebView || !m_HostView || path.empty()) {
            return false;
        }

        const std::string utf8Path = path.string();
        NSString* nsPath = [NSString stringWithUTF8String:utf8Path.c_str()];
        if (!nsPath || nsPath.length == 0) {
            return false;
        }

        BOOL isDirectory = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:nsPath isDirectory:&isDirectory] || isDirectory) {
            return false;
        }

        NSEvent* event = [NSApp currentEvent];
        if (!event) {
            return false;
        }

        if (([NSEvent pressedMouseButtons] & 1u) == 0u) {
            return false;
        }

        NSEventType eventType = [event type];
        if (eventType != NSEventTypeLeftMouseDragged &&
            eventType != NSEventTypeLeftMouseDown &&
            eventType != NSEventTypeMouseMoved &&
            eventType != NSEventTypeOtherMouseDragged) {
            return false;
        }

        NSURL* fileURL = [NSURL fileURLWithPath:nsPath isDirectory:NO];
        if (!fileURL) {
            return false;
        }

        NSDraggingItem* draggingItem = [[NSDraggingItem alloc] initWithPasteboardWriter:fileURL];
        NSImage* dragImage = [[NSWorkspace sharedWorkspace] iconForFile:nsPath];
        if (dragImage) {
            [dragImage setSize:NSMakeSize(48.0, 48.0)];
        }

        NSPoint locationInView = [m_HostView convertPoint:[event locationInWindow] fromView:nil];
        NSRect dragRect = NSMakeRect(locationInView.x - 24.0, locationInView.y - 24.0, 48.0, 48.0);
        [draggingItem setDraggingFrame:dragRect contents:dragImage];

        if (!m_DragSource) {
            m_DragSource = [[WebViewFileDragSource alloc] init];
        }

        NSDraggingSession* session = [m_HostView beginDraggingSessionWithItems:@[draggingItem]
                                                                         event:event
                                                                        source:m_DragSource];
        if (!session) {
            return false;
        }

        [session setAnimatesToStartingPositionsOnCancelOrFail:NO];
        return true;
    }

    void SetOnMouseDown(std::function<void()> callback) override {
        m_OnMouseDown = std::move(callback);
    }

    // Creation fails synchronously here: CreateWebView returns nullptr instead.
    void SetOnUnavailable(std::function<void(const std::string&)> /*callback*/) override {}

    void ConfirmBundledAppClose(std::function<void(bool)> done) override
    {
        if (!m_WebView || ![m_WebView.URL.scheme isEqualToString:@"lucia-app"])
        {
            done(true);
            return;
        }

        [m_WebView callAsyncJavaScript:
            @"return typeof window.luciaPrepareForQuit === 'function' ? await window.luciaPrepareForQuit() : true;"
            arguments:nil inFrame:nil inContentWorld:WKContentWorld.pageWorld
            completionHandler:^(id result, NSError* error) {
                if (error)
                    NSLog(@"Could not confirm OpenPencil close: %@", error);
                done(!error && [result isKindOfClass:NSNumber.class] && [result boolValue]);
            }];
    }

    void Destroy() override {
        EndMouseDrag();
        if (m_WindowResignObserver) {
            [[NSNotificationCenter defaultCenter] removeObserver:m_WindowResignObserver];
            m_WindowResignObserver = nil;
        }
        if (m_MouseMonitor) {
            [NSEvent removeMonitor:m_MouseMonitor];
            m_MouseMonitor = nil;
        }
        if (m_WebView) {
            [m_WebView setNavigationDelegate:nil];
            m_WebView.UIDelegate = nil;
            [m_WebView removeFromSuperview];
            m_WebView = nil;
        }
        if (m_NavigationDelegate) {
            m_NavigationDelegate = nil;
        }
        if (m_DragSource) {
            m_DragSource = nil;
        }
        m_Initialized = false;
    }

private:
    struct DragEventSample
    {
        CFTimeInterval EventTime;
        CFTimeInterval DispatchTime;
    };

    void EndMouseDrag()
    {
        m_ReleaseFramePriority.Reset();
        m_UncoalescedMouseInput.End();
        m_IsPrimaryPressInView = false;
        m_PointerDragActive = false;
        if (m_DragSamples.empty())
            return;

        // Save only timing data, once on release, and only for a requested local trace.
        std::string samples;
        for (const auto& [eventTime, dispatchTime] : m_DragSamples)
        {
            if (!samples.empty())
                samples += ',';
            samples += std::to_string(static_cast<long long>(eventTime * 1000.0));
            samples += '/';
            samples += std::to_string(static_cast<long long>(dispatchTime * 1000.0));
        }
        Logger::Log::Info("[BundledWebDrag] epochOffsetMs={} event/dispatch={}",
                          static_cast<long long>(m_DragEpochOffset * 1000.0), samples);
        m_DragSamples.clear();
    }

    void ObserveMouseEvent(NSEvent* event)
    {
        if (event.type == NSEventTypeLeftMouseUp)
        {
            const bool settlingBundledDrag = m_PointerDragActive &&
                event.window == m_WebView.window &&
                [m_WebView.URL.scheme isEqualToString:@"lucia-app"];
            const CFTimeInterval releaseTime = CACurrentMediaTime();
            EndMouseDrag();
            if (settlingBundledDrag)
                m_ReleaseFramePriority.Begin(releaseTime);
            return;
        }
        if (event.type == NSEventTypeLeftMouseDragged)
        {
            if (!m_IsPrimaryPressInView || event.window != m_WebView.window ||
                !m_WebView.window.isKeyWindow || m_WebView.hidden)
            {
                EndMouseDrag();
                return;
            }
            if (!m_PointerDragActive && [m_WebView.URL.scheme isEqualToString:@"lucia-app"])
            {
                m_UncoalescedMouseInput.Begin();
                if (m_TraceDragInput)
                    m_DragEpochOffset = NSDate.date.timeIntervalSince1970 - CACurrentMediaTime();
            }
            m_PointerDragActive = true;
            if (m_TraceDragInput && [m_WebView.URL.scheme isEqualToString:@"lucia-app"] &&
                m_DragSamples.size() < 1024)
                m_DragSamples.push_back({event.timestamp, CACurrentMediaTime()});
            return;
        }

        const bool overView = m_WebView && !m_WebView.hidden && event.window == m_WebView.window &&
            NSPointInRect([m_WebView convertPoint:event.locationInWindow fromView:nil], m_WebView.bounds);
        if (event.type == NSEventTypeScrollWheel)
        {
            if (overView && m_WebView.window.isKeyWindow &&
                [m_WebView.URL.scheme isEqualToString:@"lucia-app"] &&
                (event.scrollingDeltaX != 0 || event.scrollingDeltaY != 0))
            {
                // WebKit presents wheel and momentum panning independently of the editor.
                m_ReleaseFramePriority.Begin(CACurrentMediaTime());
            }
            return;
        }

        if (event.type == NSEventTypeLeftMouseDown)
        {
            EndMouseDrag();
            m_IsPrimaryPressInView = overView;
            if (overView && m_TraceDragInput && [m_WebView.URL.scheme isEqualToString:@"lucia-app"])
                m_DragSamples.reserve(1024);
        }
        if (overView && m_OnMouseDown)
            m_OnMouseDown();
    }

    Window* m_ParentWindow = nullptr;
    WKWebView* m_WebView = nil;
    BundledWebAppHandler* m_BundledApp = nil;
    NSView* m_HostView = nil;
    YouTubeNavigationDelegate* m_NavigationDelegate = nil;
    WebViewFileDragSource* m_DragSource = nil;
    id m_MouseMonitor = nil;
    id m_WindowResignObserver = nil;
    Detail::ScopedUncoalescedMouseInput m_UncoalescedMouseInput;
    Detail::NativeDragReleaseFramePriority m_ReleaseFramePriority;
    const bool m_TraceDragInput = std::getenv("GE_TRACE_BUNDLED_WEB_DRAG") != nullptr;
    CFTimeInterval m_DragEpochOffset = 0.0;
    std::vector<DragEventSample> m_DragSamples;
    std::function<void()> m_OnMouseDown;
    bool m_Initialized = false;
    bool m_IsPrimaryPressInView = false;
    bool m_PointerDragActive = false;
};

std::unique_ptr<IWebView> CreateWebView(Window* parentWindow, const std::filesystem::path& /*userDataFolder*/) {
    if (!parentWindow) {
        return nullptr;
    }

    auto webView = std::make_unique<WebView_mac>(parentWindow);
    if (!webView->GetNativeView()) {
        return nullptr;
    }

    return webView;
}

} // namespace Platform
} // namespace GameEngine

#endif // __APPLE__
