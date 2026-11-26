#if defined(_WIN32)

#include "Platform/WebView.h"
#include "Platform/Window.h"
#include "Platform/Shell.h"
#include "Utf8ToWide_Win32.h"
#include "WebViewEnvironment_Win32.h"
#include "WebViewHost_Win32.h"
#include "WebViewState.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "Logger/Logger.h"

#include <windows.h>
#include <wincrypt.h>
#include <ole2.h> // WebView2.h needs the COM declarations WIN32_LEAN_AND_MEAN leaves out

#include <WebView2.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine {
namespace Platform {

namespace {
constexpr wchar_t kBundledHost[] = L"lucia-editor.invalid";
constexpr char kBundledPage[] = "https://lucia-editor.invalid/index.html";
constexpr std::wstring_view kBundledOrigin = L"https://lucia-editor.invalid/";
using Json = nlohmann::json;

std::vector<std::function<void()>> s_WebDialogTasks;

Json SaveBundledDocument(const Json& payload)
{
    if (!payload.is_object() || !payload.contains("filename") || !payload["filename"].is_string() ||
        !payload.contains("contents") || !payload["contents"].is_string())
        return {{"error", "Invalid document data."}};

    const auto& encoded = payload["contents"].get_ref<const std::string&>();
    DWORD size = 0;
    if (encoded.size() > MAXDWORD ||
        !CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64 |
            CRYPT_STRING_STRICT, nullptr, &size, nullptr, nullptr) || !size)
        return {{"error", "Invalid document data."}};
    std::vector<BYTE> bytes(size);
    if (!CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64 |
            CRYPT_STRING_STRICT, bytes.data(), &size, nullptr, nullptr))
        return {{"error", "Invalid document data."}};

    const auto name = std::filesystem::path(Utf8ToWide(payload["filename"].get_ref<const std::string&>())).filename();
    const auto path = SaveFile(name);
    if (path.empty())
        return {{"value", nullptr}};

    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(path.parent_path().c_str(), L"lwp", 0, temporary))
        return {{"error", "Could not create a temporary document."}};
    std::ofstream output(std::filesystem::path(temporary), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(size));
    output.close();
    if (output && MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING))
        return {{"value", WideToUtf8(path.filename().c_str())}};

    DeleteFileW(temporary);
    return {{"error", "Could not save the document."}};
}
}

void PumpWebViewDialogs()
{
    // A WebView2 callback must return before a native dialog starts a nested message loop.
    auto tasks = std::exchange(s_WebDialogTasks, {});
    for (auto& task : tasks)
        task();
}

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

/**
 * The Windows web view: a WebView2 controller parented to the host window.
 *
 * - The controller is created asynchronously; WebViewState records every call made before it
 *   exists and applies them when it arrives.
 * - Completions run on the UI thread's message queue and hold the shared Handlers block, never the
 *   view: Destroy clears its owner, so a completion that arrives later does nothing.
 * - The view draws above the GPU frame, so a page that failed to load and a blank page look the
 *   same; the navigation outcome is logged.
 */
class WebView_Win32 : public IWebView, private IWebViewTarget {
public:
    WebView_Win32(HWND host, const std::filesystem::path& userDataFolder)
        : m_Host(host)
        , m_Handlers(std::make_shared<Handlers>())
    {
        m_Handlers->Owner = this;
        m_Environment = WebViewEnvironment_Win32::Acquire(userDataFolder);
        const std::shared_ptr<Handlers> handlers = m_Handlers;
        m_Environment->WhenCreated([handlers](ICoreWebView2Environment* environment, const std::string& failure) {
            if (handlers->Owner)
                handlers->Owner->OnEnvironmentCreated(environment, failure);
        });
    }

    ~WebView_Win32() override { Destroy(); }

    bool LoadURL(const std::string& url) override { return !m_Failed && m_State.LoadURL(url); }

    bool LoadBundledApp(const std::filesystem::path& directory) override
    {
        std::error_code error;
        auto root = std::filesystem::canonical(directory, error);
        if (m_Failed || m_Destroyed || error || !std::filesystem::is_regular_file(root / "index.html", error))
            return false;

        m_BundledRoot = std::move(root);
        return m_State.LoadURL(kBundledPage);
    }

    // NavigateToString takes no base URL; pages that need one arrive through LoadURL.
    bool LoadHTML(const std::string& html, const std::string& /*baseURL*/) override
    {
        return !m_Failed && m_State.LoadHTML(html);
    }

    void GoBack() override
    {
        if (m_WebView)
            m_WebView->GoBack();
    }

    void GoForward() override
    {
        if (m_WebView)
            m_WebView->GoForward();
    }

    void Reload() override
    {
        if (m_WebView)
            m_WebView->Reload();
    }

    void StopLoading() override
    {
        if (m_WebView)
            m_WebView->Stop();
    }

    WebViewNavigation GetNavigation() const override
    {
        if (!m_WebView)
            return {};

        WebViewNavigation navigation;
        BOOL canGoBack = FALSE;
        BOOL canGoForward = FALSE;
        m_WebView->get_CanGoBack(&canGoBack);
        m_WebView->get_CanGoForward(&canGoForward);
        navigation.CanGoBack = canGoBack != FALSE;
        navigation.CanGoForward = canGoForward != FALSE;
        navigation.IsLoading = m_Handlers->Loading;
        LPWSTR source = nullptr;
        if (SUCCEEDED(m_WebView->get_Source(&source)) && source)
        {
            navigation.URL = WideToUtf8(source);
            CoTaskMemFree(source);
        }
        return navigation;
    }

    void* GetNativeView() const override { return m_Destroyed ? nullptr : m_Host; }

    void Resize(int width, int height) override { m_State.Resize(width, height); }
    void SetPosition(int x, int y) override { m_State.SetPosition(x, y); }
    void SetVisible(bool visible) override { m_State.SetVisible(visible); }
    bool IsVisible() const override { return m_State.IsVisible(); }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override
    {
        m_State.SetBackgroundColor({r, g, b});
    }

    bool BeginFileDrag(const std::filesystem::path& /*path*/) override { return false; }

    // Mouse-down callback not wired on this platform; see WebView.h.
    void SetOnMouseDown(std::function<void()> /*callback*/) override {}

    void SetOnUnavailable(std::function<void(const std::string&)> callback) override
    {
        m_OnUnavailable = std::move(callback);
        if (m_OnUnavailable && m_Failed)
            m_OnUnavailable(m_Failure);
    }

    void ConfirmBundledAppClose(std::function<void(bool)> done) override
    {
        if (!IsBundledPage())
        {
            done(true);
            return;
        }

        m_CloseReply = std::move(done);
        constexpr wchar_t script[] = LR"JS(
            Promise.resolve().then(() => window.luciaPrepareForQuit?.() ?? true).then(
                result => window.chrome.webview.postMessage({ kind: 'quit', approved: result === true }),
                () => window.chrome.webview.postMessage({ kind: 'quit', approved: false }));
        )JS";
        auto handlers = m_Handlers;
        const HRESULT result = m_WebView->ExecuteScript(script,
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [handlers](HRESULT status, LPCWSTR) -> HRESULT {
                    if (FAILED(status) && handlers->Owner)
                        handlers->Owner->FinishBundledClose(false);
                    return S_OK;
                }).Get());
        if (FAILED(result))
            FinishBundledClose(false);
    }

    void Destroy() override
    {
        if (m_Destroyed)
            return;
        m_Destroyed = true;

        m_Handlers->Owner = nullptr;
        m_State.Detach();
        m_OnUnavailable = nullptr;
        if (m_Controller)
        {
            m_WebView->remove_NavigationStarting(m_NavigationStartingToken);
            m_WebView->remove_NavigationCompleted(m_NavigationCompletedToken);
            m_WebView->remove_WebMessageReceived(m_WebMessageToken);
            m_WebView->remove_ProcessFailed(m_ProcessFailedToken);
            m_Controller->Close();
        }
        m_WebView = nullptr;
        m_Controller = nullptr;
        m_Environment.reset();
        FinishBundledClose(false);
    }

private:
    struct Handlers {
        WebView_Win32* Owner = nullptr;
        // Between the latest navigation's start and its completion. WebView2 reports no loading state
        // of its own, so the navigation events keep it. A navigation that a newer one replaces
        // completes after the newer one starts, so only the latest one's completion ends loading.
        bool Loading = false;
        UINT64 LatestNavigation = 0;
    };

    bool IsBundledPage() const
    {
        if (!m_WebView || m_BundledRoot.empty())
            return false;
        LPWSTR source = nullptr;
        m_WebView->get_Source(&source);
        const bool bundled = source && std::wstring_view(source).starts_with(kBundledOrigin);
        CoTaskMemFree(source);
        return bundled;
    }

    void FinishBundledClose(bool approved)
    {
        auto reply = std::exchange(m_CloseReply, {});
        if (reply)
            reply(approved);
    }

    void OnWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args)
    {
        if (!IsBundledPage())
            return;
        LPWSTR source = nullptr;
        args->get_Source(&source);
        const bool bundled = source && std::wstring_view(source).starts_with(kBundledOrigin);
        CoTaskMemFree(source);
        if (!bundled)
            return;

        LPWSTR json = nullptr;
        if (FAILED(args->get_WebMessageAsJson(&json)) || !json)
            return;
        auto message = Json::parse(WideToUtf8(json), nullptr, false);
        CoTaskMemFree(json);
        if (!message.is_object() || !message.contains("kind") || !message["kind"].is_string())
            return;
        const auto& kind = message["kind"].get_ref<const std::string&>();
        if (kind == "quit")
        {
            FinishBundledClose(message.contains("approved") && message["approved"] == true);
            return;
        }
        if ((kind != "save" && kind != "confirm") || !message.contains("id") ||
            !message["id"].is_number_unsigned() || !message.contains("payload"))
            return;

        s_WebDialogTasks.emplace_back([handlers = m_Handlers, navigation = m_Handlers->LatestNavigation,
                                      message = std::move(message)] {
            if (handlers->Owner)
                handlers->Owner->ReplyToWebDialog(message, navigation);
        });
    }

    void ReplyToWebDialog(const Json& message, UINT64 navigation)
    {
        if (m_Handlers->LatestNavigation != navigation || !IsBundledPage())
            return;

        auto handlers = m_Handlers;
        Json reply;
        const auto& payload = message["payload"];
        if (message["kind"] == "save")
            reply = SaveBundledDocument(payload);
        else
        {
            const auto name = payload.is_string() ? payload.get<std::string>() : "Untitled";
            const int choice = ShowNativeChoiceDialog("OpenPencil", "Save changes to “" + name + "”?",
                {"Save", "Don't Save", "Cancel"}, 2);
            reply["value"] = choice == 0 ? "save" : choice == 1 ? "discard" : "cancel";
        }
        reply["id"] = message["id"];
        if (auto* owner = handlers->Owner;
            owner && handlers->LatestNavigation == navigation && owner->IsBundledPage())
            owner->m_WebView->PostWebMessageAsJson(Utf8ToWide(reply.dump()).c_str());
    }

    void OnEnvironmentCreated(ICoreWebView2Environment* environment, const std::string& failure)
    {
        if (!environment)
        {
            Fail(failure);
            return;
        }

        const std::shared_ptr<Handlers> handlers = m_Handlers;
        const HRESULT hr = environment->CreateCoreWebView2Controller(
            m_Host, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [handlers](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (handlers->Owner)
                                handlers->Owner->OnControllerCreated(controllerResult, controller);
                            return S_OK;
                        })
                        .Get());
        if (FAILED(hr))
            FailStep("CreateCoreWebView2Controller", hr);
    }

    void OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller)
    {
        if (FAILED(result) || !controller)
        {
            FailStep("the controller completion", result);
            return;
        }

        ComPtr<ICoreWebView2> webView;
        if (FAILED(controller->get_CoreWebView2(&webView)) || !webView)
        {
            controller->Close();
            FailStep("get_CoreWebView2", E_NOINTERFACE);
            return;
        }
        m_Controller = controller;
        m_WebView = webView;

        // The handlers hold the shared Handlers block, never the view, so they cannot outlive what
        // they write to.
        const std::shared_ptr<Handlers> handlers = m_Handlers;
        m_WebView->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [handlers](ICoreWebView2* /*sender*/, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    args->get_NavigationId(&handlers->LatestNavigation);
                    handlers->Loading = true;
                    if (handlers->Owner)
                        handlers->Owner->FinishBundledClose(false);
                    return S_OK;
                })
                .Get(),
            &m_NavigationStartingToken);
        m_WebView->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [handlers](ICoreWebView2* /*sender*/, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    UINT64 navigationId = 0;
                    args->get_NavigationId(&navigationId);
                    if (navigationId == handlers->LatestNavigation)
                        handlers->Loading = false;
                    BOOL succeeded = FALSE;
                    args->get_IsSuccess(&succeeded);
                    if (succeeded)
                    {
                        Logger::Log::Info("WebView: page loaded");
                        return S_OK;
                    }
                    COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    args->get_WebErrorStatus(&status);
                    Logger::Log::Error("WebView: navigation failed (WebErrorStatus {})", static_cast<int>(status));
                    return S_OK;
                })
                .Get(),
            &m_NavigationCompletedToken);

        m_State.Attach(*this);
    }

    void FailStep(const char* step, HRESULT hr)
    {
        Logger::Log::Error("WebView: {} failed (hr=0x{:08x})", step, static_cast<unsigned>(hr));
        Fail(WebViewEnvironment_Win32::RuntimeFailureMessage(hr));
    }

    // Every creation failure ends here, synchronous or not, so the caller hears of it once.
    void Fail(const std::string& failure)
    {
        m_Failed = true;
        m_Failure = failure;
        if (m_OnUnavailable)
            m_OnUnavailable(m_Failure);
    }

    void ApplyBounds(const WebViewBounds& bounds) override
    {
        m_Controller->put_Bounds(RECT{bounds.Left, bounds.Top, bounds.Right, bounds.Bottom});
    }

    void ApplyVisible(bool visible) override { m_Controller->put_IsVisible(visible ? TRUE : FALSE); }

    // The default background color arrived with the controller's second revision; an older
    // runtime keeps its white default.
    void ApplyBackgroundColor(const WebViewColor& color) override
    {
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(m_Controller.As(&controller2)) && controller2)
            controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, color.R, color.G, color.B});
    }

    HRESULT InstallBundledMessageHandlers()
    {
        auto handlers = m_Handlers;
        const HRESULT result = m_WebView->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [handlers](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    if (handlers->Owner)
                        handlers->Owner->OnWebMessage(args);
                    return S_OK;
                }).Get(), &m_WebMessageToken);
        if (FAILED(result))
            return result;
        return m_WebView->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [handlers](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
                    if (handlers->Owner)
                        handlers->Owner->FinishBundledClose(false);
                    return S_OK;
                }).Get(), &m_ProcessFailedToken);
    }

    bool NavigateToURL(const std::string& url) override
    {
        if (url == kBundledPage && !m_BundledRoot.empty())
        {
            ComPtr<ICoreWebView2_3> webView;
            HRESULT result = m_WebView.As(&webView);
            if (SUCCEEDED(result))
                result = webView->SetVirtualHostNameToFolderMapping(
                    kBundledHost, m_BundledRoot.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
            if (SUCCEEDED(result) && !m_BundledMessagesRegistered)
            {
                result = InstallBundledMessageHandlers();
                m_BundledMessagesRegistered = SUCCEEDED(result);
            }
            if (FAILED(result))
            {
                Logger::Log::Error("WebView: could not mount the bundled web app (hr=0x{:08x})",
                                   static_cast<unsigned>(result));
                Fail("Could not load the bundled web app. Update the WebView2 Runtime and restart the Editor.");
                return false;
            }
        }
        return SUCCEEDED(m_WebView->Navigate(Utf8ToWide(url).c_str()));
    }

    bool NavigateToHTML(const std::string& html) override
    {
        return SUCCEEDED(m_WebView->NavigateToString(Utf8ToWide(html).c_str()));
    }

    HWND m_Host = nullptr;
    std::shared_ptr<Handlers> m_Handlers;
    std::shared_ptr<WebViewEnvironment_Win32> m_Environment;
    ComPtr<ICoreWebView2Controller> m_Controller;
    ComPtr<ICoreWebView2> m_WebView;
    EventRegistrationToken m_NavigationStartingToken{};
    EventRegistrationToken m_NavigationCompletedToken{};
    EventRegistrationToken m_WebMessageToken{};
    EventRegistrationToken m_ProcessFailedToken{};
    WebViewState m_State;
    std::function<void(const std::string&)> m_OnUnavailable;
    std::filesystem::path m_BundledRoot;
    std::function<void(bool)> m_CloseReply;
    bool m_BundledMessagesRegistered = false;
    std::string m_Failure;
    bool m_Failed = false;
    bool m_Destroyed = false;
};

std::unique_ptr<IWebView> CreateWebView(Window* parentWindow, const std::filesystem::path& userDataFolder)
{
    GLFWwindow* glfwWindow = parentWindow ? parentWindow->GetGLFWHandle() : nullptr;
    HWND host = glfwWindow ? glfwGetWin32Window(glfwWindow) : nullptr;
    if (!host)
        return nullptr;

    return std::make_unique<WebView_Win32>(host, userDataFolder);
}

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
