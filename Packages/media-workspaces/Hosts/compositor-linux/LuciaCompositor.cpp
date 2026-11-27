#include "ui/CanvasWidget.h"
#include "ui/MainWindow.h"
#include "rendering/LayerRenderer.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QMenuBar>
#include <QMessageBox>
#include <QTimer>
#include <QtConcurrentRun>

#include "../QtWorkspaceHost.h"

#include <cstdint>
#include <memory>

namespace
{
using namespace LuciaMedia;

class CompositorWorkspace
{
public:
    CompositorWorkspace(const char* assetsRoot, TexturePreview preview)
        : m_AssetsRoot(QFileInfo(QString::fromUtf8(assetsRoot)).canonicalFilePath())
        , m_Preview(preview)
    {
        auto* menu = m_Window.menuBar()->addMenu(QStringLiteral("Live Texture"));
        auto* paint = menu->addAction(QStringLiteral("Paint Project PNG…"));
        auto* stop = menu->addAction(QStringLiteral("Stop Live Preview"));
        paint->setEnabled(!m_AssetsRoot.isEmpty());
        QObject::connect(paint, &QAction::triggered, &m_Window, [this] { OpenTexture(); });
        QObject::connect(stop, &QAction::triggered, &m_Window, [this] { ClearPreview(); });
        QObject::connect(m_Window.canvas(), &compositor::CanvasWidget::luciaDocumentChanged, &m_Window,
            [this] { m_Dirty = true; });
        QObject::connect(&m_Render, &QFutureWatcher<QImage>::finished, &m_Window, [this] { PublishFrame(); });
        QObject::connect(&m_PreviewTimer, &QTimer::timeout, &m_Window, [this] { StartRender(); });
        m_PreviewTimer.start(16);
    }

    ~CompositorWorkspace()
    {
        m_PreviewTimer.stop();
        m_Render.disconnect(&m_Window);
        m_Render.waitForFinished();
        ClearPreview();
        Place(nullptr, 0, 0, 0, 0);
    }

    void Place(void* host, double x, double y, double width, double height)
    {
        m_Host.Place(host, x, y, width, height);
    }

    bool ConfirmClose() { return m_Window.close(); }

private:
    void OpenTexture()
    {
        const auto path = QFileDialog::getOpenFileName(&m_Window, QStringLiteral("Paint Project PNG"),
            m_AssetsRoot, QStringLiteral("PNG images (*.png)"));
        if (path.isEmpty())
            return;
        const auto canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.startsWith(m_AssetsRoot + QLatin1Char('/')))
        {
            QMessageBox::warning(&m_Window, QStringLiteral("Live Texture"),
                                 QStringLiteral("Choose a PNG inside the project's Assets folder."));
            return;
        }
        if (!m_Window.openLuciaTexture(canonical))
            return;
        ClearPreview();
        m_TexturePath = canonical.toUtf8();
        m_TextureDocument = m_Window.document();
        // The original texture is already bound. The first edit publishes the first frame.
        m_Dirty = false;
    }

    void ClearPreview()
    {
        if (!m_TexturePath.isEmpty())
            m_Preview(m_TexturePath.constData(), nullptr, 0, 0, 0);
        m_TexturePath.clear();
        m_TextureDocument.reset();
        ++m_Binding;
    }

    void StartRender()
    {
        if (m_TexturePath.isEmpty())
            return;
        const auto document = m_TextureDocument.lock();
        if (m_Window.document() != document || !document)
        {
            ClearPreview();
            return;
        }
        if (!m_Dirty || m_RenderPending || !m_Window.isVisible())
            return;
        m_Dirty = false;
        m_RenderPending = true;
        m_RenderBinding = m_Binding;
        m_Render.setFuture(QtConcurrent::run([snapshot = *document] {
            return compositor::LayerRenderer::flattened(snapshot).convertToFormat(QImage::Format_RGBA8888);
        }));
    }

    void PublishFrame()
    {
        m_RenderPending = false;
        if (m_RenderBinding != m_Binding || m_TexturePath.isEmpty() ||
            m_Window.document() != m_TextureDocument.lock())
            return;
        const auto frame = m_Render.result();
        if (!frame.isNull())
            m_Preview(m_TexturePath.constData(), frame.constBits(), static_cast<uint32_t>(frame.width()),
                      static_cast<uint32_t>(frame.height()), static_cast<size_t>(frame.sizeInBytes()));
    }

    compositor::MainWindow m_Window;
    QtWorkspaceHost m_Host{m_Window};
    QString m_AssetsRoot;
    TexturePreview m_Preview;
    QByteArray m_TexturePath;
    std::weak_ptr<compositor::Document> m_TextureDocument;
    QFutureWatcher<QImage> m_Render;
    QTimer m_PreviewTimer;
    uint64_t m_Binding = 0;
    uint64_t m_RenderBinding = 0;
    bool m_Dirty = false;
    bool m_RenderPending = false;
};
}

extern "C" {
Q_DECL_EXPORT void* LuciaCreateProjectMediaWorkspace(const char* bundle, const char* assetsRoot, TexturePreview preview)
{
    if (!bundle || !assetsRoot || !preview || !EnsureQtApplication(QString::fromUtf8(bundle)))
        return nullptr;
    const QDir bundledModels(QString::fromUtf8(bundle) + QStringLiteral("/models"));
    if (!qEnvironmentVariableIsSet("COMPOSITOR_U2NETP_MODEL"))
        qputenv("COMPOSITOR_U2NETP_MODEL", bundledModels.filePath(QStringLiteral("u2netp.onnx")).toUtf8());
    if (!qEnvironmentVariableIsSet("COMPOSITOR_DEPTH_MODEL"))
        qputenv("COMPOSITOR_DEPTH_MODEL", bundledModels.filePath(QStringLiteral("depthmini.onnx")).toUtf8());
    return new CompositorWorkspace(assetsRoot, preview);
}

Q_DECL_EXPORT void LuciaDestroyMediaWorkspace(void* workspace)
{
    delete static_cast<CompositorWorkspace*>(workspace);
}

Q_DECL_EXPORT void LuciaPlaceMediaWorkspace(void* workspace, void* host,
                                             double x, double y, double width, double height)
{
    static_cast<CompositorWorkspace*>(workspace)->Place(host, x, y, width, height);
}

Q_DECL_EXPORT void LuciaConfirmMediaWorkspaceClose(void* workspace, void* context, CloseReply reply)
{
    reply(context, static_cast<CompositorWorkspace*>(workspace)->ConfirmClose() ? 1 : 0);
}

Q_DECL_EXPORT void LuciaPumpMediaWorkspaceEvents()
{
    PumpQtEvents();
}
}
