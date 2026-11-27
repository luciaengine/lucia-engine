#pragma once

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QWidget>

#include <cstdint>

namespace LuciaMedia
{
using TexturePreview = void (*)(const char*, const uint8_t*, uint32_t, uint32_t, size_t);
using CloseReply = void (*)(void*, int32_t);

inline bool EnsureQtApplication(const QString& workspaceDirectory)
{
    QCoreApplication::addLibraryPath(workspaceDirectory);
    QCoreApplication::addLibraryPath(QDir(workspaceDirectory).filePath(QStringLiteral("plugins")));
    if (!QCoreApplication::instance())
    {
        // Qt owns platform objects that remain active until editor process exit.
        static char name[] = "LuciaMediaWorkspace";
#if defined(__linux__)
        static char platform[] = "-platform";
        static char backend[] = "xcb";
        static char* arguments[] = {name, platform, backend, nullptr};
        static int count = 3;
#else
        static char* arguments[] = {name, nullptr};
        static int count = 1;
#endif
        auto* app = new QApplication(count, arguments);
        app->setApplicationName(QStringLiteral("Lucia Media Workspaces"));
        app->setOrganizationName(QStringLiteral("Lucia Engine"));
        app->setQuitOnLastWindowClosed(false);
    }
    return qobject_cast<QApplication*>(QCoreApplication::instance()) != nullptr;
}

inline void PumpQtEvents()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

class QtWorkspaceHost
{
public:
    explicit QtWorkspaceHost(QWidget& window);
    ~QtWorkspaceHost();

    void Place(void* host, double x, double y, double width, double height);

private:
    void Hide();

    QWidget& m_Window;
    void* m_Display = nullptr;
    void* m_Host = nullptr;
    QRect m_Bounds;
};
}
