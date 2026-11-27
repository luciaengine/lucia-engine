#include "RoomDocument.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>
#include <limits>

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "%s\n", message);
    return condition;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;

    const auto source = directory.filePath(QStringLiteral("original.mp4"));
    QFile media(source);
    if (!media.open(QIODevice::WriteOnly) || media.write("source bytes") != 12)
        return 1;
    media.close();

    const QJsonObject probe = QJsonDocument::fromJson(R"JSON({
        "streams": [{"codec_type":"video", "width":64, "height":48, "avg_frame_rate":"30/1"},
                    {"codec_type":"audio"}],
        "format": {"duration":"2"}
    })JSON").object();
    LuciaMedia::RoomDocument document;
    QString error;
    const auto asset = document.Import(source, probe, error);
    if (!Check(!asset.isEmpty(), "The source asset is accepted"))
        return 1;
    document.AddClip(asset, 0);
    if (!Check(document.Clips().size() == 2, "Video and audio arrive as linked clips"))
        return 1;

    const auto original = document.Clips();
    if (!Check(!document.Split(0, 2) && document.Clips() == original, "An invalid split keeps both linked clips"))
        return 1;
    if (!Check(!document.Split(0, std::numeric_limits<double>::quiet_NaN()) && document.Clips() == original,
               "A non-finite split keeps both linked clips"))
        return 1;

    const auto lockedPath = directory.filePath(QStringLiteral("locked.room"));
    if (!document.Save(lockedPath, error))
        return 1;
    QFile lockedFile(lockedPath);
    if (!lockedFile.open(QIODevice::ReadOnly))
        return 1;
    auto lockedJson = QJsonDocument::fromJson(lockedFile.readAll()).object();
    lockedFile.close();
    auto lockedProject = lockedJson.value("project").toObject();
    auto audio = lockedProject.value("audioTracks").toArray();
    auto track = audio.first().toObject();
    track.insert("locked", true);
    audio[0] = track;
    lockedProject.insert("audioTracks", audio);
    lockedJson.insert("project", lockedProject);
    if (!lockedFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        lockedFile.write(QJsonDocument(lockedJson).toJson()) < 0)
        return 1;
    lockedFile.close();

    LuciaMedia::RoomDocument lockedDocument;
    if (!Check(lockedDocument.Open(lockedPath, error) && !lockedDocument.Split(0, 0.5) &&
               lockedDocument.Clips() == original, "A locked audio track keeps both linked clips intact"))
        return 1;
    if (!Check(document.Split(0, 0.5) && document.Clips().size() == 4,
               "Splitting a linked clip splits its audio"))
        return 1;
    if (!Check(document.SetClipRange(1, 0.75, 0.5, 0.5) && document.Duration() == 1.25,
               "Trimming the second section updates the timeline duration"))
        return 1;

    const auto path = directory.filePath(QStringLiteral("edit.room"));
    if (!Check(document.Save(path, error), "The timeline saves in the Editroom project format"))
        return 1;
    LuciaMedia::RoomDocument reopened;
    if (!Check(reopened.Open(path, error) && reopened.Clips() == document.Clips(),
               "The saved project preserves timeline clips"))
        return 1;

    QString filters;
    const auto arguments = reopened.RenderArguments(directory.filePath(QStringLiteral("export.mp4")),
        directory.filePath(QStringLiteral("filters.txt")), filters, error);
    if (!Check(arguments.contains("-/filter_complex") && filters.contains("adelay=36000S") && filters.contains("overlay="),
               "The render plan preserves video, audio, and the timeline gap"))
        return 1;
    const auto second = directory.filePath(QStringLiteral("second.mp4"));
    if (!QFile::copy(source, second) || reopened.Import(second, probe, error).isEmpty() ||
        !reopened.Save(path, error))
        return 1;
    const auto linkedClips = reopened.Clips();
    const auto relocated = directory.filePath(QStringLiteral("Relocated"));
    if (!QDir().mkpath(relocated) ||
        !QFile::rename(source, QDir(relocated).filePath(QStringLiteral("renamed.mp4"))) ||
        !QFile::rename(second, QDir(relocated).filePath(QStringLiteral("second.mp4"))) ||
        !reopened.Open(path, error))
        return 1;
    if (!Check(reopened.MissingAssets().size() == 2, "Opening a project identifies missing media") ||
        !Check(!reopened.RelinkMissingAsset(asset, relocated), "A folder cannot replace a media file") ||
        !Check(reopened.RelinkMissingAsset(asset, QDir(relocated).filePath(QStringLiteral("renamed.mp4"))),
               "An explicitly selected replacement relinks the missing asset") ||
        !Check(reopened.MissingAssets().isEmpty(), "Missing sibling files are relinked from the same folder") ||
        !Check(reopened.Clips() == linkedClips, "Relinking preserves timeline edits and asset IDs") ||
        !Check(reopened.Save(path, error) && reopened.Open(path, error) && reopened.MissingAssets().isEmpty(),
               "Relinked paths persist when the project reopens"))
        return 1;
    return 0;
}
