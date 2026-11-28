#include "WebViewGtkModule.h"
#include "WebViewState.h"

#include "Platform/WebView.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <X11/Xlib.h>

namespace GameEngine {
namespace Platform {
namespace {

constexpr char kBundledScheme[] = "lucia-app";
constexpr char kBundledOrigin[] = "lucia-app://editor/";
constexpr char kBundledRootKey[] = "lucia-bundled-web-root";

bool IsBundledUrl(const char* uri)
{
    return uri && std::string_view(uri).starts_with(kBundledOrigin);
}

void ServeBundledAsset(WebKitURISchemeRequest* request, gpointer)
{
    auto* webView = webkit_uri_scheme_request_get_web_view(request);
    const auto* root = webView ? static_cast<const std::filesystem::path*>(
        g_object_get_data(G_OBJECT(webView), kBundledRootKey)) : nullptr;
    const char* path = webkit_uri_scheme_request_get_path(request);
    const char* method = webkit_uri_scheme_request_get_http_method(request);
    std::unique_ptr<char, decltype(&g_free)> decoded(path ? g_uri_unescape_string(path, nullptr) : nullptr, &g_free);
    std::error_code error;
    if (root && IsBundledUrl(webkit_uri_scheme_request_get_uri(request)) &&
        g_strcmp0(method, "GET") == 0 && decoded && decoded.get()[0] == '/')
    {
        const auto relative = std::filesystem::path(decoded.get() + 1);
        const auto file = std::filesystem::canonical(*root / (relative.empty() ? "index.html" : relative), error);
        const auto within = file.lexically_relative(*root);
        if (!error && !within.empty() && !within.is_absolute() && *within.begin() != ".." &&
            std::filesystem::is_regular_file(file, error))
        {
            GFile* source = g_file_new_for_path(file.c_str());
            GFileInputStream* stream = g_file_read(source, nullptr, nullptr);
            g_object_unref(source);
            if (stream)
            {
                const auto extension = file.extension();
                const char* mime = extension == ".wasm" ? "application/wasm" :
                    (extension == ".js" || extension == ".mjs") ? "text/javascript" : nullptr;
                std::unique_ptr<char, decltype(&g_free)> guessed(
                    mime ? nullptr : g_content_type_guess(file.c_str(), nullptr, 0, nullptr), &g_free);
                std::unique_ptr<char, decltype(&g_free)> mimeType(
                    guessed ? g_content_type_get_mime_type(guessed.get()) : nullptr, &g_free);
                WebKitURISchemeResponse* response = webkit_uri_scheme_response_new(G_INPUT_STREAM(stream), -1);
                const char* contentType = mime ? mime : mimeType ? mimeType.get() : "application/octet-stream";
                webkit_uri_scheme_response_set_content_type(response, contentType);
                SoupMessageHeaders* headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
                soup_message_headers_append(headers, "Content-Type", contentType);
                soup_message_headers_append(headers, "Content-Security-Policy", "frame-src 'none'");
                soup_message_headers_append(headers, "Cross-Origin-Opener-Policy", "same-origin");
                soup_message_headers_append(headers, "Cross-Origin-Embedder-Policy", "require-corp");
                webkit_uri_scheme_response_set_http_headers(response, headers);
                webkit_uri_scheme_request_finish_with_response(request, response);
                g_object_unref(response);
                g_object_unref(stream);
                return;
            }
        }
    }
    GError* failure = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Bundled web asset not found");
    webkit_uri_scheme_request_finish_error(request, failure);
    g_error_free(failure);
}

void RegisterBundledScheme(WebKitWebContext* context)
{
    if (g_object_get_data(G_OBJECT(context), kBundledRootKey))
        return;

    webkit_web_context_register_uri_scheme(context, kBundledScheme, &ServeBundledAsset, nullptr, nullptr);
    auto* security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_secure(security, kBundledScheme);
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, kBundledScheme);
    g_object_set_data(G_OBJECT(context), kBundledRootKey, context);
}

constexpr char kDocumentSavePromptKey[] = "lucia-web-document-save-prompt";
constexpr char kDocumentClosePromptKey[] = "lucia-web-document-close-prompt";

gboolean SaveBundledDocument(WebKitUserContentManager*, JSCValue* payload,
                              WebKitScriptMessageReply* reply, gpointer data)
{
    if (!IsBundledUrl(webkit_web_view_get_uri(WEBKIT_WEB_VIEW(data))))
    {
        webkit_script_message_reply_return_error_message(reply, "The bundled app is unavailable.");
        return TRUE;
    }

    auto* context = jsc_value_get_context(payload);
    auto* filename = jsc_value_is_object(payload) ? jsc_value_object_get_property(payload, "filename") : nullptr;
    auto* contents = jsc_value_is_object(payload) ? jsc_value_object_get_property(payload, "contents") : nullptr;
    std::unique_ptr<char, decltype(&g_free)> name(
        filename && jsc_value_is_string(filename) ? jsc_value_to_string(filename) : nullptr, &g_free);
    std::unique_ptr<char, decltype(&g_free)> base64(
        contents && jsc_value_is_string(contents) ? jsc_value_to_string(contents) : nullptr, &g_free);
    g_clear_object(&filename);
    g_clear_object(&contents);
    gsize length = 0;
    std::unique_ptr<guchar, decltype(&g_free)> bytes(base64 ? g_base64_decode(base64.get(), &length) : nullptr,
                                                   &g_free);
    if (!name || length == 0)
    {
        webkit_script_message_reply_return_error_message(reply, "Invalid document data.");
        return TRUE;
    }

    auto* prompt = static_cast<DocumentSavePrompt*>(g_object_get_data(G_OBJECT(data), kDocumentSavePromptKey));
    if (prompt && *prompt)
    {
        auto pendingReply = std::shared_ptr<WebKitScriptMessageReply>(
            webkit_script_message_reply_ref(reply), webkit_script_message_reply_unref);
        auto pendingContext = std::shared_ptr<JSCContext>(JSC_CONTEXT(g_object_ref(context)), g_object_unref);
        auto pendingBytes = std::shared_ptr<guchar>(bytes.release(), g_free);
        (*prompt)(name.get(), [pendingReply, pendingContext, pendingBytes, length](std::filesystem::path path) {
            if (path.empty())
            {
                auto* result = jsc_value_new_null(pendingContext.get());
                webkit_script_message_reply_return_value(pendingReply.get(), result);
                g_object_unref(result);
                return;
            }
            GFile* file = g_file_new_for_path(path.c_str());
            const bool saved = g_file_replace_contents(file, reinterpret_cast<const char*>(pendingBytes.get()),
                length, nullptr, FALSE, G_FILE_CREATE_NONE, nullptr, nullptr, nullptr);
            g_object_unref(file);
            if (!saved)
            {
                webkit_script_message_reply_return_error_message(pendingReply.get(), "Could not save the document.");
                return;
            }
            auto* result = jsc_value_new_string(pendingContext.get(), path.filename().c_str());
            webkit_script_message_reply_return_value(pendingReply.get(), result);
            g_object_unref(result);
        });
        return TRUE;
    }

    GtkWidget* dialog = gtk_file_chooser_dialog_new("Save Document", nullptr, GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    auto* chooser = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
    gtk_file_chooser_set_current_name(chooser, std::filesystem::path(name.get()).filename().c_str());
    JSCValue* result = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    {
        GFile* file = gtk_file_chooser_get_file(chooser);
        if (file && g_file_replace_contents(file, reinterpret_cast<const char*>(bytes.get()), length, nullptr,
                                            FALSE, G_FILE_CREATE_NONE, nullptr, nullptr, nullptr))
        {
            std::unique_ptr<char, decltype(&g_free)> savedName(g_file_get_basename(file), &g_free);
            result = jsc_value_new_string(context, savedName.get());
        }
        g_clear_object(&file);
    }
    else
        result = jsc_value_new_null(context);
    gtk_widget_destroy(dialog);
    if (result)
    {
        webkit_script_message_reply_return_value(reply, result);
        g_object_unref(result);
    }
    else
        webkit_script_message_reply_return_error_message(reply, "Could not save the document.");
    return TRUE;
}

gboolean ChooseBundledDocumentClose(WebKitUserContentManager*, JSCValue* name,
                                     WebKitScriptMessageReply* reply, gpointer data)
{
    if (!IsBundledUrl(webkit_web_view_get_uri(WEBKIT_WEB_VIEW(data))))
    {
        webkit_script_message_reply_return_error_message(reply, "The bundled app is unavailable.");
        return TRUE;
    }

    std::unique_ptr<char, decltype(&g_free)> filename(
        jsc_value_is_string(name) ? jsc_value_to_string(name) : nullptr, &g_free);
    auto* prompt = static_cast<DocumentClosePrompt*>(g_object_get_data(G_OBJECT(data), kDocumentClosePromptKey));
    if (prompt && *prompt)
    {
        auto pendingReply = std::shared_ptr<WebKitScriptMessageReply>(
            webkit_script_message_reply_ref(reply), webkit_script_message_reply_unref);
        auto* context = jsc_value_get_context(name);
        auto pendingContext = std::shared_ptr<JSCContext>(JSC_CONTEXT(g_object_ref(context)), g_object_unref);
        (*prompt)(filename ? filename.get() : "Untitled", [pendingReply, pendingContext](DocumentCloseChoice choice) {
            auto* result = jsc_value_new_string(pendingContext.get(), DocumentCloseChoiceName(choice));
            webkit_script_message_reply_return_value(pendingReply.get(), result);
            g_object_unref(result);
        });
        return TRUE;
    }

    GtkWidget* dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE, "Save changes to “%s”?", filename ? filename.get() : "Untitled");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Don’t Save", GTK_RESPONSE_REJECT, "_Cancel",
                           GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    const int choice = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    JSCValue* result = jsc_value_new_string(jsc_value_get_context(name),
        choice == GTK_RESPONSE_ACCEPT ? "save" : choice == GTK_RESPONSE_REJECT ? "discard" : "cancel");
    webkit_script_message_reply_return_value(reply, result);
    g_object_unref(result);
    return TRUE;
}

void OnBundledCloseResult(GObject* source, GAsyncResult* result, gpointer data)
{
    std::unique_ptr<std::function<void(bool)>> done(static_cast<std::function<void(bool)>*>(data));
    JSCValue* value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(source), result, nullptr);
    const bool approved = value && jsc_value_is_boolean(value) && jsc_value_to_boolean(value);
    g_clear_object(&value);
    (*done)(approved);
}

/// GTK events run per host frame, at most. A frame that leaves events waiting runs them next frame,
/// so a busy page cannot stall the editor.
constexpr int kMaxEventsPerFrame = 64;

enum class GtkStart {
    NotTried,
    Ready,
    Failed
};

GtkStart s_GtkStart = GtkStart::NotTried;

/// GTK starts on its X11 backend, on the display GLFW uses ($DISPLAY): on a Wayland desktop the
/// editor's window lives on XWayland, and only a window of the same X server can be its child.
bool StartGtk()
{
    if (s_GtkStart == GtkStart::NotTried)
    {
        gdk_set_allowed_backends("x11");
        s_GtkStart = gtk_init_check(nullptr, nullptr) ? GtkStart::Ready : GtkStart::Failed;
        if (s_GtkStart == GtkStart::Failed)
            Logger::Log::Error("WebView: GTK could not open the X11 display; the web view is not available");
    }
    return s_GtkStart == GtkStart::Ready;
}

/// The Linux web view: a WebKitWebView in an override-redirect GTK popup whose X11 window is a child
/// of the host window. Bounds are host pixels; GTK takes pixels divided by the GDK scale factor. A
/// press in the page takes the X11 input focus, and ReturnFocus gives it back on a press in the host.
class WebView_Gtk : public IWebView, private IWebViewTarget {
public:
    explicit WebView_Gtk(unsigned long parentWindow)
        : m_ParentWindow(parentWindow)
    {
        m_Window = gtk_window_new(GTK_WINDOW_POPUP);
        auto* manager = webkit_user_content_manager_new();
        m_WebView = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(manager));
        g_object_unref(manager);
        gtk_container_add(GTK_CONTAINER(m_Window), GTK_WIDGET(m_WebView));
        gtk_widget_show(GTK_WIDGET(m_WebView));

        g_signal_connect(m_WebView, "button-press-event", G_CALLBACK(&WebView_Gtk::OnButtonPress), this);
        g_signal_connect(m_WebView, "load-changed", G_CALLBACK(&WebView_Gtk::OnLoadChanged), this);
        g_signal_connect(m_WebView, "load-failed", G_CALLBACK(&WebView_Gtk::OnLoadFailed), this);
        g_signal_connect(m_Window, "focus-out-event", G_CALLBACK(&WebView_Gtk::OnFocusOut), this);

        gtk_widget_realize(m_Window);
        GdkWindow* gdkWindow = gtk_widget_get_window(m_Window);
        XReparentWindow(XDisplay(), gdk_x11_window_get_xid(gdkWindow), m_ParentWindow, 0, 0);
        XFlush(XDisplay());

        m_State.Attach(*this);
    }

    ~WebView_Gtk() override { Destroy(); }

    bool LoadURL(const std::string& url) override { return m_Window && m_State.LoadURL(url); }

    bool LoadBundledApp(const std::filesystem::path& directory) override
    {
        std::error_code error;
        auto root = std::filesystem::canonical(directory, error);
        if (!m_Window || error || !std::filesystem::is_regular_file(root / "index.html", error))
            return false;

        RegisterBundledScheme(webkit_web_view_get_context(m_WebView));
        if (!g_object_get_data(G_OBJECT(m_WebView), kBundledRootKey))
        {
            auto* manager = webkit_web_view_get_user_content_manager(m_WebView);
            g_signal_connect(manager, "script-message-with-reply-received::luciaSaveDocument",
                             G_CALLBACK(&SaveBundledDocument), m_WebView);
            g_signal_connect(manager, "script-message-with-reply-received::luciaChooseDocumentClose",
                             G_CALLBACK(&ChooseBundledDocumentClose), m_WebView);
            if (!webkit_user_content_manager_register_script_message_handler_with_reply(
                    manager, "luciaSaveDocument", nullptr) ||
                !webkit_user_content_manager_register_script_message_handler_with_reply(
                    manager, "luciaChooseDocumentClose", nullptr))
                return false;
        }
        g_object_set_data_full(G_OBJECT(m_WebView), kBundledRootKey, new std::filesystem::path(std::move(root)),
            [](gpointer value) { delete static_cast<std::filesystem::path*>(value); });
        return LoadURL(kBundledOrigin);
    }

    bool LoadHTML(const std::string& html, const std::string& baseURL) override
    {
        if (!m_Window)
            return false;
        m_BaseURL = baseURL;
        return m_State.LoadHTML(html);
    }

    void GoBack() override
    {
        if (m_Window)
            webkit_web_view_go_back(m_WebView);
    }

    void GoForward() override
    {
        if (m_Window)
            webkit_web_view_go_forward(m_WebView);
    }

    void Reload() override
    {
        if (m_Window)
            webkit_web_view_reload(m_WebView);
    }

    void StopLoading() override
    {
        if (m_Window)
            webkit_web_view_stop_loading(m_WebView);
    }

    WebViewNavigation GetNavigation() const override
    {
        if (!m_Window)
            return {};
        const gchar* uri = webkit_web_view_get_uri(m_WebView);
        return {webkit_web_view_can_go_back(m_WebView) != FALSE, webkit_web_view_can_go_forward(m_WebView) != FALSE,
                webkit_web_view_is_loading(m_WebView) != FALSE, uri ? uri : ""};
    }

    void* GetNativeView() const override { return m_Window ? m_WebView : nullptr; }

    void Resize(int width, int height) override { m_State.Resize(width, height); }
    void SetPosition(int x, int y) override { m_State.SetPosition(x, y); }
    void SetVisible(bool visible) override { m_State.SetVisible(visible); }
    bool IsVisible() const override { return m_State.IsVisible(); }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override
    {
        m_State.SetBackgroundColor({r, g, b});
    }

    bool BeginFileDrag(const std::filesystem::path& /*path*/) override { return false; }

    void SetOnMouseDown(std::function<void()> callback) override { m_OnMouseDown = std::move(callback); }

    // GTK creation is synchronous: a view that cannot start is never returned.
    void SetOnUnavailable(std::function<void(const std::string&)> /*callback*/) override {}

    void SetDocumentClosePrompt(DocumentClosePrompt prompt) override
    {
        m_DocumentClosePrompt = std::move(prompt);
        if (m_WebView)
            g_object_set_data(G_OBJECT(m_WebView), kDocumentClosePromptKey, &m_DocumentClosePrompt);
    }

    void SetDocumentSavePrompt(DocumentSavePrompt prompt) override
    {
        m_DocumentSavePrompt = std::move(prompt);
        if (m_WebView)
            g_object_set_data(G_OBJECT(m_WebView), kDocumentSavePromptKey, &m_DocumentSavePrompt);
    }

    void RunBundledScript(const std::string& script, std::function<void(std::string)> done) override
    {
        if (!m_WebView || !IsBundledUrl(webkit_web_view_get_uri(m_WebView)))
        {
            done({});
            return;
        }
        webkit_web_view_evaluate_javascript(m_WebView, script.c_str(), -1, nullptr, nullptr, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer context) {
                std::unique_ptr<std::function<void(std::string)>> reply(
                    static_cast<std::function<void(std::string)>*>(context));
                auto* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, nullptr);
                char* json = value ? jsc_value_to_json(value, 0) : nullptr;
                std::string text = json ? json : "";
                g_free(json);
                if (value)
                    g_object_unref(value);
                (*reply)(std::move(text));
            }, new std::function<void(std::string)>(std::move(done)));
    }

    void ConfirmBundledAppClose(std::function<void(bool)> done) override
    {
        if (!m_Window || !IsBundledUrl(webkit_web_view_get_uri(m_WebView)))
        {
            done(true);
            return;
        }

        constexpr char script[] =
            "return typeof window.luciaPrepareForQuit === 'function' ? await window.luciaPrepareForQuit() : true;";
        webkit_web_view_call_async_javascript_function(m_WebView, script, -1, nullptr, nullptr, nullptr, nullptr,
            &OnBundledCloseResult, new std::function<void(bool)>(std::move(done)));
    }

    void Destroy() override
    {
        if (!m_Window)
            return;

        if (s_FocusedView == this)
            s_FocusedView = nullptr;
        m_State.Detach();
        m_OnMouseDown = nullptr;
        g_signal_handlers_disconnect_by_data(m_WebView, this);
        g_signal_handlers_disconnect_by_data(webkit_web_view_get_user_content_manager(m_WebView), m_WebView);
        g_signal_handlers_disconnect_by_data(m_Window, this);

        // The host window may already be gone, and X11 destroys a child window with its parent.
        GdkDisplay* display = gtk_widget_get_display(m_Window);
        gdk_x11_display_error_trap_push(display);
        gtk_widget_destroy(m_Window);
        gdk_x11_display_error_trap_pop_ignored(display);
        m_Window = nullptr;
        m_WebView = nullptr;
    }

    /// The host saw a press outside the page.
    static void ReturnFocus(unsigned long parentWindow)
    {
        if (!s_FocusedView || s_FocusedView->m_ParentWindow != parentWindow)
            return;

        s_FocusedView = nullptr;
        Display* display = XDisplay();
        XSetInputFocus(display, parentWindow, RevertToParent, CurrentTime);
        XFlush(display);
    }

private:
    static Display* XDisplay() { return gdk_x11_display_get_xdisplay(gdk_display_get_default()); }

    int ScaleFactor() const { return gtk_widget_get_scale_factor(m_Window); }

    void ApplyBounds(const WebViewBounds& bounds) override
    {
        const int scale = ScaleFactor();
        gtk_window_move(GTK_WINDOW(m_Window), bounds.Left / scale, bounds.Top / scale);
        gtk_window_resize(GTK_WINDOW(m_Window), std::max(1, (bounds.Right - bounds.Left) / scale),
                          std::max(1, (bounds.Bottom - bounds.Top) / scale));
    }

    void ApplyVisible(bool visible) override
    {
        if (visible)
            gtk_widget_show(m_Window);
        else
            gtk_widget_hide(m_Window);
    }

    void ApplyBackgroundColor(const WebViewColor& color) override
    {
        const GdkRGBA rgba{color.R / 255.0, color.G / 255.0, color.B / 255.0, 1.0};
        webkit_web_view_set_background_color(m_WebView, &rgba);
    }

    bool NavigateToURL(const std::string& url) override
    {
        webkit_web_view_load_uri(m_WebView, url.c_str());
        return true;
    }

    bool NavigateToHTML(const std::string& html) override
    {
        webkit_web_view_load_html(m_WebView, html.c_str(), m_BaseURL.empty() ? nullptr : m_BaseURL.c_str());
        return true;
    }

    /// Takes the keyboard focus for the page, then tells the host. Returns FALSE so the page also
    /// receives the press.
    static gboolean OnButtonPress(GtkWidget* /*widget*/, GdkEventButton* /*event*/, gpointer self)
    {
        auto* view = static_cast<WebView_Gtk*>(self);
        if (s_FocusedView != view)
        {
            Display* display = XDisplay();
            GdkDisplay* gdkDisplay = gtk_widget_get_display(view->m_Window);
            gdk_x11_display_error_trap_push(gdkDisplay);
            XSetInputFocus(display, gdk_x11_window_get_xid(gtk_widget_get_window(view->m_Window)), RevertToParent,
                           CurrentTime);
            gdk_x11_display_error_trap_pop_ignored(gdkDisplay);
            s_FocusedView = view;
        }
        gtk_widget_grab_focus(GTK_WIDGET(view->m_WebView));
        if (view->m_OnMouseDown)
            view->m_OnMouseDown();
        return FALSE;
    }

    static gboolean OnFocusOut(GtkWidget* /*widget*/, GdkEventFocus* /*event*/, gpointer self)
    {
        if (s_FocusedView == self)
            s_FocusedView = nullptr;
        return FALSE;
    }

    static void OnLoadChanged(WebKitWebView* /*webView*/, WebKitLoadEvent loadEvent, gpointer self)
    {
        auto* view = static_cast<WebView_Gtk*>(self);
        if (loadEvent == WEBKIT_LOAD_STARTED)
            view->m_LoadFailed = false;
        else if (loadEvent == WEBKIT_LOAD_FINISHED && !view->m_LoadFailed)
            Logger::Log::Info("WebView: page loaded");
    }

    static gboolean OnLoadFailed(WebKitWebView* /*webView*/, WebKitLoadEvent /*loadEvent*/, gchar* uri,
                                 GError* error, gpointer self)
    {
        static_cast<WebView_Gtk*>(self)->m_LoadFailed = true;
        // A load the user stopped, and a link that became a download, end here without failing.
        if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) ||
            g_error_matches(error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE))
            return FALSE;
        Logger::Log::Error("WebView: navigation to '{}' failed ({})", uri ? uri : "", error ? error->message : "");
        return FALSE;
    }

    /// The view that took the X11 input focus with a press in its page, until it loses it.
    static inline WebView_Gtk* s_FocusedView = nullptr;

    unsigned long m_ParentWindow = 0;
    GtkWidget* m_Window = nullptr;
    WebKitWebView* m_WebView = nullptr;
    WebViewState m_State;
    std::string m_BaseURL;
    DocumentClosePrompt m_DocumentClosePrompt;
    DocumentSavePrompt m_DocumentSavePrompt;
    std::function<void()> m_OnMouseDown;
    bool m_LoadFailed = false;
};

std::unique_ptr<IWebView> CreateGtkWebView(unsigned long parentWindow)
{
    if (!StartGtk())
        return nullptr;
    return std::make_unique<WebView_Gtk>(parentWindow);
}

void PumpGtkEvents()
{
    if (s_GtkStart != GtkStart::Ready)
        return;
    for (int i = 0; i < kMaxEventsPerFrame && g_main_context_iteration(nullptr, FALSE); ++i)
    {
    }
}

constexpr WebViewGtkModule kModule{&CreateGtkWebView, &PumpGtkEvents, &WebView_Gtk::ReturnFocus};

} // namespace
} // namespace Platform
} // namespace GameEngine

extern "C" __attribute__((visibility("default"))) const GameEngine::Platform::WebViewGtkModule* GeWebViewGtkModule()
{
    return &GameEngine::Platform::kModule;
}
