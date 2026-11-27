#include "QtWorkspaceHost.h"

#if defined(__linux__)
#include <X11/Xlib.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace LuciaMedia
{
QtWorkspaceHost::QtWorkspaceHost(QWidget& window) : m_Window(window)
{
    m_Window.setWindowFlag(Qt::FramelessWindowHint);
#if defined(__linux__)
    m_Window.setWindowFlag(Qt::X11BypassWindowManagerHint);
    m_Display = XOpenDisplay(nullptr);
#endif
    m_Window.winId();
}

QtWorkspaceHost::~QtWorkspaceHost()
{
    Hide();
#if defined(__linux__)
    if (m_Display)
        XCloseDisplay(static_cast<Display*>(m_Display));
#endif
}

void QtWorkspaceHost::Place(void* host, double x, double y, double width, double height)
{
    if (!host || width <= 0 || height <= 0)
    {
        Hide();
        return;
    }
    const QRect bounds(qRound(x), qRound(y), qRound(width), qRound(height));
    if (m_Host == host && m_Bounds == bounds)
        return;
    if (m_Host != host)
        Hide();

#if defined(__linux__)
    auto* display = static_cast<Display*>(m_Display);
    if (!display)
        return;
    const auto child = static_cast<Window>(m_Window.winId());
    if (!m_Host)
        XReparentWindow(display, child, reinterpret_cast<Window>(host), bounds.x(), bounds.y());
    XMoveResizeWindow(display, child, bounds.x(), bounds.y(), bounds.width(), bounds.height());
    XSync(display, False);
#elif defined(_WIN32)
    const auto child = reinterpret_cast<HWND>(m_Window.winId());
    if (!m_Host)
    {
        const auto flags = GetWindowLongPtrW(child, GWL_STYLE);
        SetWindowLongPtrW(child, GWL_STYLE, (flags & ~WS_POPUP) | WS_CHILD);
        SetParent(child, static_cast<HWND>(host));
    }
    SetWindowPos(child, nullptr, bounds.x(), bounds.y(), bounds.width(), bounds.height(),
                 SWP_NOACTIVATE | SWP_NOZORDER);
#endif

    m_Host = host;
    m_Bounds = bounds;
    if (!m_Window.isVisible())
        m_Window.show();
}

void QtWorkspaceHost::Hide()
{
    if (!m_Host)
        return;

    m_Window.hide();
#if defined(__linux__)
    auto* display = static_cast<Display*>(m_Display);
    XReparentWindow(display, static_cast<Window>(m_Window.winId()), DefaultRootWindow(display), 0, 0);
    XFlush(display);
#elif defined(_WIN32)
    SetParent(reinterpret_cast<HWND>(m_Window.winId()), nullptr);
#endif
    m_Host = nullptr;
    m_Bounds = {};
}
}
