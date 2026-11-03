#include "Platform/Window.h"
#include "Logger/Logger.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <vector>

namespace GameEngine::Platform
{
namespace
{
std::vector<Window*> s_Windows;
bool s_Active = true;
bool s_Quit = false;

SDL_Window* Native(void* handle) { return static_cast<SDL_Window*>(handle); }
int KeyCode(SDL_Keycode key)
{
    if (key >= SDLK_A && key <= SDLK_Z) return static_cast<int>(key - SDLK_A + 65);
    if (key >= SDLK_0 && key <= SDLK_9) return static_cast<int>(key);
    switch (key)
    {
    case SDLK_ESCAPE: return 256;
    case SDLK_RETURN: return 257;
    case SDLK_TAB: return 258;
    case SDLK_BACKSPACE: return 259;
    case SDLK_RIGHT: return 262;
    case SDLK_LEFT: return 263;
    case SDLK_DOWN: return 264;
    case SDLK_UP: return 265;
    case SDLK_SPACE: return 32;
    default: return -1;
    }
}
}
Window::~Window() { Destroy(); }
bool Window::Initialize()
{
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        Logger::Log::Error("Mobile window initialization failed: {}", SDL_GetError());
        return false;
    }
    return true;
}
bool Window::Create(const WindowDesc& desc)
{
    if (!Initialize()) return false;
#if defined(GE_PLATFORM_IOS)
    constexpr SDL_WindowFlags graphics = SDL_WINDOW_METAL;
#else
    constexpr SDL_WindowFlags graphics = SDL_WINDOW_VULKAN;
#endif
    auto* window = SDL_CreateWindow(desc.Title.c_str(), desc.Width, desc.Height,
        graphics | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_FULLSCREEN);
    if (!window)
    {
        Logger::Log::Error("Mobile window creation failed: {}", SDL_GetError());
        return false;
    }
    m_MobileHandle = window;
    s_Windows.push_back(this);
    m_IsFullscreen = true;
    m_CurrentTitle = desc.Title;
    SDL_SetPointerProperty(SDL_GetWindowProperties(window), "openengine.window", this);
    return true;
}
void Window::Destroy()
{
    std::erase(s_Windows, this);
    if (m_MobileHandle) SDL_DestroyWindow(Native(m_MobileHandle));
    m_MobileHandle = nullptr;
}
void Window::Terminate() { SDL_Quit(); }
void Window::DispatchMobileEvent(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_QUIT)
    {
        s_Quit = true;
        for (auto* window : s_Windows) window->m_MobileShouldClose = true;
        return;
    }
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND || event.type == SDL_EVENT_DID_ENTER_FOREGROUND)
    {
        s_Active = event.type == SDL_EVENT_DID_ENTER_FOREGROUND;
        for (auto* window : s_Windows)
        {
            if (!s_Active && window->m_MobilePointerDown && window->m_MouseButton)
                window->m_MouseButton(0, false, 0);
            window->m_MobilePointerDown = false;
            if (window->m_Focus) window->m_Focus(s_Active);
        }
        return;
    }
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
    {
        if (auto* gamepad = SDL_GetGamepadFromID(event.gdevice.which)) SDL_CloseGamepad(gamepad);
        return;
    }
    SDL_WindowID id = 0;
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) id = event.window.windowID;
    else if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_MOTION) id = event.tfinger.windowID;
    else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) id = event.key.windowID;
    else if (event.type == SDL_EVENT_MOUSE_MOTION) id = event.motion.windowID;
    else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) id = event.button.windowID;
    auto* window = SDL_GetWindowFromID(id);
    if (!window) return;
    auto* self = static_cast<Window*>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), "openengine.window", nullptr));
    if (!self) return;
    switch (event.type)
    {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: self->m_MobileShouldClose = true; break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (self->m_FramebufferSize) self->m_FramebufferSize(event.window.data1, event.window.data2);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (self->m_Focus) self->m_Focus(event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (self->m_Key) self->m_Key(KeyCode(event.key.key), event.key.down ? (event.key.repeat ? 2 : 1) : 0, 0);
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    {
        if (event.type == SDL_EVENT_FINGER_DOWN)
        {
            if (self->m_MobilePointerDown) break;
            self->m_MobilePointerDown = true;
            self->m_MobileFinger = event.tfinger.fingerID;
        }
        else if (!self->m_MobilePointerDown || self->m_MobileFinger != event.tfinger.fingerID) break;
        if (event.type == SDL_EVENT_FINGER_UP) self->m_MobilePointerDown = false;
        int width = 0, height = 0;
        SDL_GetWindowSize(window, &width, &height);
        if (self->m_MouseMove) self->m_MouseMove(event.tfinger.x * width, event.tfinger.y * height);
        if (event.type != SDL_EVENT_FINGER_MOTION && self->m_MouseButton)
            self->m_MouseButton(0, event.type == SDL_EVENT_FINGER_DOWN, 0);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (self->m_MouseMove) self->m_MouseMove(event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (self->m_MouseButton) self->m_MouseButton(event.button.button - 1, event.button.down, 0);
        break;
    default: break;
    }
}
void Window::PollEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) DispatchMobileEvent(event);
    // Stop simulation and GPU work while the OS owns the backgrounded application.
    while (!s_Active && !s_Quit)
        if (SDL_WaitEventTimeout(&event, 100)) DispatchMobileEvent(event);
}
void* Window::GetNativeHandle() const { return m_MobileHandle; }
GLFWwindow* Window::GetGLFWHandle() const { return nullptr; }
void Window::GetFramebufferSize(int& w, int& h) const { SDL_GetWindowSizeInPixels(Native(m_MobileHandle), &w, &h); }
void Window::GetWindowSize(int& w, int& h) const { SDL_GetWindowSize(Native(m_MobileHandle), &w, &h); }
void Window::SetWindowSize(int w, int h) { SDL_SetWindowSize(Native(m_MobileHandle), w, h); }
bool Window::IsFocused() const { return (SDL_GetWindowFlags(Native(m_MobileHandle)) & SDL_WINDOW_INPUT_FOCUS) != 0; }
bool Window::IsKeyPressed(int key) const
{
    const bool* keys = SDL_GetKeyboardState(nullptr);
    SDL_Keycode sdlKey = key >= 65 && key <= 90 ? SDLK_A + key - 65 : static_cast<SDL_Keycode>(key);
    return keys[SDL_GetScancodeFromKey(sdlKey, nullptr)];
}
void Window::GetContentScale(float& x, float& y) const { x = y = SDL_GetWindowDisplayScale(Native(m_MobileHandle)); }
void Window::GetCursorClientPosition(float& x, float& y) const { SDL_GetMouseState(&x, &y); }
bool Window::ShouldClose() const { return m_MobileShouldClose; }
void Window::RequestClose() { m_MobileShouldClose = true; }
void Window::SetTitle(const std::string& title) { m_CurrentTitle = title; SDL_SetWindowTitle(Native(m_MobileHandle), title.c_str()); }
void Window::GetPosition(int& x, int& y) const { SDL_GetWindowPosition(Native(m_MobileHandle), &x, &y); }
void Window::SetPosition(int x, int y) { SDL_SetWindowPosition(Native(m_MobileHandle), x, y); }
void Window::GetFrameSize(int& l, int& t, int& r, int& b) const { l = t = r = b = 0; }
void Window::Focus() { SDL_RaiseWindow(Native(m_MobileHandle)); }
void Window::Show() { SDL_ShowWindow(Native(m_MobileHandle)); }
void Window::Hide() { SDL_HideWindow(Native(m_MobileHandle)); }
void Window::SetOwnedBy(Window*) {}
void Window::SetDecorated(bool) {}
void Window::SetResizable(bool) {}
void Window::SetToolWindowStyle(bool) {}
void Window::SetShowInTaskbar(bool) {}
void Window::SetAlwaysOnTop(bool) {}
void Window::SetIcon(const unsigned char*, int, int) {}
void Window::SetTitleBarRightClickHandler(TitleBarRightClickHandler handler) { m_TitleBarRightClick = std::move(handler); }
void Window::FireTitleBarRightClick(int x, int y) { if (m_TitleBarRightClick) m_TitleBarRightClick(x, y); }
void Window::GetCursorScreenPosition(int& x, int& y) { float fx = 0, fy = 0; SDL_GetMouseState(&fx, &fy); x = static_cast<int>(fx); y = static_cast<int>(fy); }
bool Window::IsLeftMouseButtonDown() { return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0; }
bool Window::IsOptionKeyDown() { return (SDL_GetModState() & SDL_KMOD_ALT) != 0; }
int Window::GetModifierKeyMask() { return 0; }
uint32_t Window::GetScreenPixelColor(int, int) { return 0; }
void Window::StartGlobalClickMonitor(GlobalClickCallback) {}
void Window::StopGlobalClickMonitor() {}
void Window::StartGlobalMoveMonitor(GlobalMoveCallback) {}
void Window::StopGlobalMoveMonitor() {}
void Window::PushGlobalCrosshairCursor() {}
void Window::PopGlobalCursor() {}
void Window::SetCursor(CursorType) {}
void Window::ResetCursor() {}
void Window::SetCursorLocked(bool locked) { m_CursorLocked = SDL_SetWindowRelativeMouseMode(Native(m_MobileHandle), locked) && locked; }
void Window::SetFullscreen(bool) {}
}
