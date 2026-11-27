#include "RoomDocument.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>

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
    return 0;
}
