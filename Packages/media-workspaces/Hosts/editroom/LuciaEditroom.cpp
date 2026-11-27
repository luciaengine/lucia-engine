#include "../QtWorkspaceHost.h"
#include "RoomDocument.h"

#include <QAction>
#include <QAudioOutput>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonDocument>
#include <QListWidget>
#include <QMainWindow>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QProcess>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QUrl>
#include <QVideoWidget>
#include <QVBoxLayout>

#include <functional>
#include <memory>
#include <utility>

namespace LuciaMedia
{
namespace
{
constexpr int kStartColumn = 2;
constexpr int kSourceColumn = 3;
constexpr int kDurationColumn = 4;

class EditroomWorkspace
{
public:
    explicit EditroomWorkspace(QString bundle) : m_Bundle(std::move(bundle)), m_Host(m_Window)
    {
        m_Window.setWindowTitle(QStringLiteral("Editroom"));
        m_Window.resize(1200, 800);
        auto* file = m_Window.addToolBar(QStringLiteral("Project"));
        AddAction(file, "New", QKeySequence::New, [this] { NewProject(); });
        AddAction(file, "Open…", QKeySequence::Open, [this] { OpenProject(); });
        AddAction(file, "Save", QKeySequence::Save, [this] { SaveProject(); });
        AddAction(file, "Import Media…", QKeySequence(QStringLiteral("Ctrl+I")), [this] { Import(); });
        AddAction(file, "Export…", QKeySequence(), [this] { Export(); });
        AddAction(file, "Cancel Render", QKeySequence(), [this] { CancelRender(); });

        auto* panes = new QSplitter(Qt::Vertical, &m_Window);
        auto* top = new QSplitter(Qt::Horizontal, panes);
        m_Media = new QListWidget(top);
        m_Media->setAccessibleName(QStringLiteral("Media"));
        auto* preview = new QWidget(top);
        auto* previewLayout = new QVBoxLayout(preview);
        auto* video = new QVideoWidget(preview);
        previewLayout->addWidget(video, 1);
        m_Player.setVideoOutput(video);
        m_Player.setAudioOutput(&m_Audio);
        auto* playback = new QToolBar(QStringLiteral("Playback"), preview);
        AddAction(playback, "Play from Start", QKeySequence(), [this] { Play(true); });
        AddAction(playback, "Play / Pause", QKeySequence(Qt::Key_Space), [this] { Play(false); });
        auto* position = new QSlider(Qt::Horizontal, playback);
        position->setAccessibleName(QStringLiteral("Playhead"));
        position->setRange(0, 0);
        playback->addWidget(position);
        previewLayout->addWidget(playback);
        top->setStretchFactor(1, 1);

        auto* timeline = new QWidget(panes);
        auto* layout = new QVBoxLayout(timeline);
        auto* commands = new QToolBar(QStringLiteral("Timeline"), timeline);
        AddAction(commands, "Add to Timeline", QKeySequence(), [this] { AddClip(); });
        AddAction(commands, "Split", QKeySequence(QStringLiteral("Ctrl+B")), [this] { Split(); });
        AddAction(commands, "Delete", QKeySequence(Qt::Key_Delete), [this] { Delete(); });
        layout->addWidget(commands);
        m_Clips = new QTableWidget(timeline);
        m_Clips->setColumnCount(5);
        m_Clips->setHorizontalHeaderLabels({"Media", "Track Type", "Start", "Source In", "Duration"});
        m_Clips->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_Clips->setSelectionMode(QAbstractItemView::SingleSelection);
        m_Clips->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        layout->addWidget(m_Clips);
        panes->setStretchFactor(0, 2);
        panes->setStretchFactor(1, 1);
        m_Window.setCentralWidget(panes);

        QObject::connect(m_Media, &QListWidget::itemDoubleClicked, &m_Window,
            [this] { AddClip(); });
        QObject::connect(m_Clips, &QTableWidget::cellChanged, &m_Window,
            [this](int row, int column) { if (column >= kStartColumn) SetRange(row); });
        QObject::connect(position, &QSlider::sliderMoved, &m_Window,
            [this](int time) { m_Playhead = time / 1000.0; m_Player.setPosition(time); });
        QObject::connect(&m_Player, &QMediaPlayer::positionChanged, &m_Window,
            [this, position](qint64 time) {
                m_Playhead = time / 1000.0;
                if (!position->isSliderDown())
                    position->setValue(static_cast<int>(time));
            });
        QObject::connect(&m_Player, &QMediaPlayer::durationChanged, &m_Window,
            [position](qint64 duration) { position->setMaximum(static_cast<int>(duration)); });
        QObject::connect(&m_Player, &QMediaPlayer::mediaStatusChanged, &m_Window,
            [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::LoadedMedia && m_StartPreview)
                {
                    m_StartPreview = false;
                    m_Player.setPosition(qRound64(m_PreviewStart * 1000));
                    m_Player.play();
                }
            });
        QObject::connect(&m_Renderer, &QProcess::finished, &m_Window,
            [this](int exitCode, QProcess::ExitStatus status) { FinishRender(exitCode, status); });
        QObject::connect(&m_Renderer, &QProcess::errorOccurred, &m_Window,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                ShowError(m_Renderer.errorString());
                m_Render.reset();
            });
    }

    ~EditroomWorkspace()
    {
        CancelRender();
        m_Player.setSource(QUrl());
    }

    void Place(void* host, double x, double y, double width, double height)
    {
        m_Host.Place(host, x, y, width, height);
    }

    bool ConfirmClose()
    {
        if (!ConfirmReplace())
            return false;
        CancelRender();
        m_Player.stop();
        return true;
    }

private:
    void AddAction(QToolBar* toolbar, const char* label, const QKeySequence& shortcut,
                   std::function<void()> invoke)
    {
        auto* action = toolbar->addAction(QString::fromUtf8(label));
        action->setShortcut(shortcut);
        QObject::connect(action, &QAction::triggered, &m_Window, std::move(invoke));
    }

    void ShowError(const QString& error)
    {
        QMessageBox::warning(&m_Window, QStringLiteral("Editroom"), error);
    }

    QString Tool(const QString& name) const
    {
#if defined(_WIN32)
        const auto executable = name + QStringLiteral(".exe");
#else
        const auto executable = name;
#endif
        const auto bundled = QDir(m_Bundle).absoluteFilePath(executable);
        return QFileInfo(bundled).isExecutable() ? bundled : QStandardPaths::findExecutable(executable);
    }

    void UpdateTitle()
    {
        const auto title = m_Path.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(m_Path).completeBaseName();
        m_Window.setWindowTitle(title + (m_Dirty ? QStringLiteral(" *") : QString()) + QStringLiteral(" — Editroom"));
    }

    bool ConfirmReplace()
    {
        if (!m_Dirty)
            return true;
        const auto choice = QMessageBox::question(&m_Window, QStringLiteral("Editroom"),
            QStringLiteral("Save changes to this project?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        return choice == QMessageBox::Discard || (choice == QMessageBox::Save && SaveProject());
    }

    void ResetPlayback()
    {
        CancelRender();
        m_Player.setSource(QUrl());
        m_Playhead = 0;
        m_PreviewCurrent = false;
        m_StartPreview = false;
        m_Preview.reset();
    }

    void NewProject()
    {
        if (!ConfirmReplace())
            return;
        ResetPlayback();
        m_Document = RoomDocument();
        m_Path.clear();
        m_Dirty = false;
        Refresh();
    }

    void OpenProject()
    {
        if (!ConfirmReplace())
            return;
        const auto path = QFileDialog::getOpenFileName(&m_Window, QStringLiteral("Open Editroom Project"),
            m_Path, QStringLiteral("Editroom projects (*.room *.cutplay)"));
        if (path.isEmpty())
            return;
        QString error;
        if (!m_Document.Open(path, error))
        {
            ShowError(error);
            return;
        }
        ResetPlayback();
        m_Path = path;
        m_Dirty = false;
        Refresh();
    }

    bool SaveProject()
    {
        auto path = m_Path;
        if (path.isEmpty())
            path = QFileDialog::getSaveFileName(&m_Window, QStringLiteral("Save Editroom Project"),
                QStringLiteral("Untitled.room"), QStringLiteral("Editroom projects (*.room)"));
        if (path.isEmpty())
            return false;
        if (QFileInfo(path).suffix().isEmpty())
            path += QStringLiteral(".room");
        QString error;
        if (!m_Document.Save(path, error))
        {
            ShowError(error);
            return false;
        }
        m_Path = path;
        m_Dirty = false;
        UpdateTitle();
        return true;
    }

    void Import()
    {
        const auto probe = Tool(QStringLiteral("ffprobe"));
        if (probe.isEmpty())
        {
            ShowError(QStringLiteral("Install FFmpeg to import media."));
            return;
        }
        const auto paths = QFileDialog::getOpenFileNames(&m_Window, QStringLiteral("Import Media"));
        for (const auto& path : paths)
        {
            QProcess process;
            process.start(probe, {"-v", "error", "-show_streams", "-show_format", "-of", "json", path});
            if (!process.waitForFinished(20000) || process.exitCode() != 0)
            {
                ShowError(QStringLiteral("Could not read %1.").arg(QFileInfo(path).fileName()));
                continue;
            }
            QString error;
            if (m_Document.Import(path, QJsonDocument::fromJson(process.readAllStandardOutput()).object(), error).isEmpty())
                ShowError(error);
            else
                m_Dirty = true;
        }
        Refresh();
    }

    void AddClip()
    {
        const auto* item = m_Media->currentItem();
        if (!item)
            return;
        m_Document.AddClip(item->data(Qt::UserRole).toString(), m_Document.Duration());
        Changed();
    }

    void Split()
    {
        if (m_Clips->currentRow() < 0)
            return;
        if (!m_Document.Split(m_Clips->currentRow(), m_Playhead))
        {
            ShowError(QStringLiteral("Put the playhead inside a clip that supports splitting."));
            return;
        }
        Changed();
    }

    void Delete()
    {
        if (m_Clips->currentRow() < 0)
            return;
        m_Document.DeleteClip(m_Clips->currentRow());
        Changed();
    }

    void SetRange(int row)
    {
        bool okStart = false, okSource = false, okDuration = false;
        const double start = m_Clips->item(row, kStartColumn)->text().toDouble(&okStart);
        const double source = m_Clips->item(row, kSourceColumn)->text().toDouble(&okSource);
        const double duration = m_Clips->item(row, kDurationColumn)->text().toDouble(&okDuration);
        if (okStart && okSource && okDuration && m_Document.SetClipRange(row, start, source, duration))
            Changed();
        else
        {
            Refresh();
            m_Window.statusBar()->showMessage(QStringLiteral("Choose a valid clip range."), 3500);
        }
    }

    void Changed()
    {
        m_Dirty = true;
        m_PreviewCurrent = false;
        m_Player.pause();
        Refresh();
    }

    void Refresh()
    {
        UpdateTitle();
        const auto row = m_Clips->currentRow();
        const QSignalBlocker block(m_Clips);
        m_Media->clear();
        for (const auto& value : m_Document.Assets())
        {
            const auto asset = value.toObject();
            auto* item = new QListWidgetItem(asset.value("name").toString(), m_Media);
            item->setData(Qt::UserRole, asset.value("id").toString());
        }
        const auto clips = m_Document.Clips();
        m_Clips->setRowCount(static_cast<int>(clips.size()));
        for (int i = 0; i < clips.size(); ++i)
        {
            const auto clip = clips[i].toObject();
            const auto asset = m_Document.Asset(clip.value("assetID").toString());
            auto* name = new QTableWidgetItem(asset.value("name").toString());
            auto* kind = new QTableWidgetItem(clip.value("kind").toString());
            name->setFlags(name->flags() & ~Qt::ItemIsEditable);
            kind->setFlags(kind->flags() & ~Qt::ItemIsEditable);
            m_Clips->setItem(i, 0, name);
            m_Clips->setItem(i, 1, kind);
            m_Clips->setItem(i, kStartColumn, new QTableWidgetItem(QString::number(clip.value("start").toDouble())));
            m_Clips->setItem(i, kSourceColumn, new QTableWidgetItem(QString::number(clip.value("sourceStart").toDouble())));
            m_Clips->setItem(i, kDurationColumn, new QTableWidgetItem(QString::number(clip.value("duration").toDouble())));
        }
        if (row >= 0 && row < m_Clips->rowCount())
            m_Clips->selectRow(row);
    }

    void Play(bool fromStart)
    {
        if (!fromStart && m_Player.playbackState() == QMediaPlayer::PlayingState)
        {
            m_Player.pause();
            return;
        }
        if (fromStart)
            m_Playhead = 0;
        if (m_PreviewCurrent)
        {
            m_Player.setPosition(qRound64(m_Playhead * 1000));
            m_Player.play();
            return;
        }
        m_PreviewStart = m_Playhead;
        Render(QString());
    }

    void Export()
    {
        auto destination = QFileDialog::getSaveFileName(&m_Window, QStringLiteral("Export Video"),
            QStringLiteral("Editroom.mp4"), QStringLiteral("MP4 video (*.mp4)"));
        if (destination.isEmpty())
            return;
        if (QFileInfo(destination).suffix().isEmpty())
            destination += QStringLiteral(".mp4");
        Render(destination);
    }

    struct Job
    {
        QTemporaryDir Directory;
        QString Destination;
        QString Output;
        QJsonArray Clips;
        bool Cancelled = false;
    };

    void Render(const QString& destination)
    {
        if (m_Render)
        {
            m_Window.statusBar()->showMessage(QStringLiteral("A render is already running."), 3500);
            return;
        }
        const auto ffmpeg = Tool(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty())
        {
            ShowError(QStringLiteral("Install FFmpeg to preview or export the timeline."));
            return;
        }
        auto job = std::make_unique<Job>();
        job->Destination = destination;
        job->Clips = m_Document.Clips();
        job->Output = job->Directory.filePath(QStringLiteral("render.mp4"));
        const auto script = job->Directory.filePath(QStringLiteral("filters.txt"));
        QString error, filters;
        const auto arguments = m_Document.RenderArguments(job->Output, script, filters, error);
        if (arguments.isEmpty())
        {
            ShowError(error);
            return;
        }
        QFile manifest(script);
        const auto graph = filters.toUtf8();
        if (!job->Directory.isValid() || !manifest.open(QIODevice::WriteOnly) || manifest.write(graph) != graph.size())
        {
            ShowError(QStringLiteral("Could not prepare the timeline render."));
            return;
        }
        manifest.close();
        m_Render = std::move(job);
        m_Window.statusBar()->showMessage(destination.isEmpty() ? QStringLiteral("Preparing preview…") :
            QStringLiteral("Exporting video…"));
        m_Renderer.start(ffmpeg, arguments);
    }

    void CancelRender()
    {
        if (!m_Render)
            return;
        m_Render->Cancelled = true;
        m_Renderer.kill();
        m_Renderer.waitForFinished();
        m_Render.reset();
    }

    void FinishRender(int code, QProcess::ExitStatus status)
    {
        auto job = std::move(m_Render);
        if (!job || job->Cancelled)
        {
            m_Window.statusBar()->clearMessage();
            return;
        }
        if (status != QProcess::NormalExit || code != 0)
        {
            ShowError(QString::fromUtf8(m_Renderer.readAllStandardError()).right(1000));
            return;
        }
        if (job->Destination.isEmpty())
        {
            if (job->Clips != m_Document.Clips())
                return;
            m_Player.setSource(QUrl());
            m_Preview = std::move(job);
            m_PreviewCurrent = true;
            m_StartPreview = true;
            m_Player.setSource(QUrl::fromLocalFile(m_Preview->Output));
            m_Window.statusBar()->clearMessage();
            return;
        }
        QFile source(job->Output);
        QSaveFile destination(job->Destination);
        if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly))
        {
            ShowError(QStringLiteral("Could not open the export destination."));
            return;
        }
        while (!source.atEnd())
        {
            const auto chunk = source.read(1024 * 1024);
            if (chunk.isEmpty() || destination.write(chunk) != chunk.size())
            {
                ShowError(QStringLiteral("Could not save the rendered video."));
                return;
            }
        }
        if (!destination.commit())
            ShowError(destination.errorString());
        else
            m_Window.statusBar()->showMessage(QStringLiteral("Export complete."), 5000);
    }

    QString m_Bundle;
    QMainWindow m_Window;
    QtWorkspaceHost m_Host;
    QAudioOutput m_Audio;
    QMediaPlayer m_Player;
    QProcess m_Renderer;
    RoomDocument m_Document;
    std::unique_ptr<Job> m_Render;
    std::unique_ptr<Job> m_Preview;
    QListWidget* m_Media = nullptr;
    QTableWidget* m_Clips = nullptr;
    QString m_Path;
    double m_Playhead = 0;
    double m_PreviewStart = 0;
    bool m_Dirty = false;
    bool m_PreviewCurrent = false;
    bool m_StartPreview = false;
};
}
}

extern "C" {
Q_DECL_EXPORT void* LuciaCreateProjectMediaWorkspace(const char* bundle, const char*, LuciaMedia::TexturePreview)
{
    if (!bundle || !LuciaMedia::EnsureQtApplication(QString::fromUtf8(bundle)))
        return nullptr;
    return new LuciaMedia::EditroomWorkspace(QString::fromUtf8(bundle));
}

Q_DECL_EXPORT void LuciaDestroyMediaWorkspace(void* workspace)
{
    delete static_cast<LuciaMedia::EditroomWorkspace*>(workspace);
}

Q_DECL_EXPORT void LuciaPlaceMediaWorkspace(void* workspace, void* host,
                                            double x, double y, double width, double height)
{
    static_cast<LuciaMedia::EditroomWorkspace*>(workspace)->Place(host, x, y, width, height);
}

Q_DECL_EXPORT void LuciaConfirmMediaWorkspaceClose(void* workspace, void* context, LuciaMedia::CloseReply reply)
{
    reply(context, static_cast<LuciaMedia::EditroomWorkspace*>(workspace)->ConfirmClose() ? 1 : 0);
}

Q_DECL_EXPORT void LuciaPumpMediaWorkspaceEvents()
{
    LuciaMedia::PumpQtEvents();
}
}
