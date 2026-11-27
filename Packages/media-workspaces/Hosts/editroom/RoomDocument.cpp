#include "RoomDocument.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace LuciaMedia
{
namespace
{
QString NewId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
}

QJsonObject Track(const QString& kind)
{
    return {{"id", NewId()}, {"kind", kind}, {"hidden", false}, {"muted", false}, {"gainDB", 0}};
}

QString Number(double value)
{
    return QString::number(value, 'f', 9);
}

QJsonObject NewClip(const QJsonObject& asset, const QJsonObject& track, double start)
{
    return {{"id", NewId()}, {"assetID", asset.value("id")}, {"trackID", track.value("id")},
            {"kind", track.value("kind")}, {"start", start}, {"duration", asset.value("duration")},
            {"sourceStart", 0}, {"speed", 1}, {"fadeIn", 0}, {"fadeOut", 0}, {"gainDB", 0},
            {"color", QJsonObject{{"r", 0.16}, {"g", 0.16}, {"b", 0.2}, {"a", 1}}},
            {"text", QJsonObject{{"string", "Text"}, {"fontName", ""}, {"bold", true}, {"size", 110},
                {"color", QJsonObject{{"r", 1}, {"g", 1}, {"b", 1}, {"a", 1}}}, {"x", 0.5}, {"y", 0.5}}}};
}

bool HasAdvancedTiming(const QJsonObject& clip)
{
    return clip.value("speed").toDouble(1) != 1 || clip.value("reversed").toBool() ||
           clip.value("freeze").toBool() || clip.value("ramp").isObject() ||
           clip.value("reversedFrom").isObject();
}

bool HasAdvancedRendering(const QJsonObject& clip)
{
    return HasAdvancedTiming(clip) || clip.value("transitionIn").isObject() ||
           clip.value("grade").isObject() || clip.value("transform").isObject() ||
           clip.value("fadeIn").toDouble() != 0 || clip.value("fadeOut").toDouble() != 0;
}

bool InBounds(qsizetype index, const QJsonArray& array)
{
    return index >= 0 && index < array.size();
}
}

RoomDocument::RoomDocument()
{
    m_Document.insert("version", 1);
    SetProject({{"width", 1920}, {"height", 1080}, {"fps", 30}, {"settingsLocked", false},
                {"videoTracks", QJsonArray{Track("video")}}, {"audioTracks", QJsonArray{Track("audio")}},
                {"clips", QJsonArray()}, {"assets", QJsonArray()}});
}

QJsonObject RoomDocument::Project() const { return m_Document.value("project").toObject(); }
void RoomDocument::SetProject(QJsonObject project) { m_Document.insert("project", std::move(project)); }
QJsonArray RoomDocument::Assets() const { return Project().value("assets").toArray(); }
QJsonArray RoomDocument::Clips() const { return Project().value("clips").toArray(); }

bool RoomDocument::Open(const QString& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = file.errorString();
        return false;
    }
    const auto document = QJsonDocument::fromJson(file.readAll()).object();
    const auto project = document.value("project").toObject();
    if (document.value("version").toInt() != 1 || !project.value("assets").isArray() ||
        !project.value("clips").isArray() || !project.value("videoTracks").isArray() ||
        !project.value("audioTracks").isArray())
    {
        error = QStringLiteral("This file is not a supported Editroom project.");
        return false;
    }
    m_Path = path;
    m_Document = document;
    return true;
}

bool RoomDocument::Save(const QString& path, QString& error)
{
    QJsonArray assets;
    const QDir directory = QFileInfo(path).absoluteDir();
    for (const auto& value : Assets())
    {
        auto asset = value.toObject();
        const auto media = MediaPath(asset);
        if (!media.isEmpty())
        {
            asset.insert("path", media);
            asset.insert("relativePath", directory.relativeFilePath(media));
        }
        assets.append(asset);
    }
    auto project = Project();
    project.insert("assets", assets);
    auto document = m_Document;
    document.insert("project", project);
    const QByteArray bytes = QJsonDocument(document).toJson();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        error = file.errorString();
        return false;
    }
    m_Document = document;
    m_Path = path;
    return true;
}

QString RoomDocument::Import(const QString& path, const QJsonObject& probe, QString& error)
{
    QJsonObject video;
    bool hasAudio = false;
    for (const auto& value : probe.value("streams").toArray())
    {
        const auto stream = value.toObject();
        const auto kind = stream.value("codec_type").toString();
        if (kind == "video" && !stream.value("disposition").toObject().value("attached_pic").toInt() &&
            video.isEmpty())
            video = stream;
        if (kind == "audio")
            hasAudio = true;
    }
    const double duration = probe.value("format").toObject().value("duration").toString().toDouble();
    if ((!hasAudio && video.isEmpty()) || !std::isfinite(duration) || duration <= 0)
    {
        error = QStringLiteral("Choose a video or audio file with a readable duration.");
        return {};
    }
    const auto rate = video.value("avg_frame_rate").toString().split('/');
    const double divisor = rate.size() == 2 ? rate[1].toDouble() : 0;
    const double fps = divisor > 0 ? rate[0].toDouble() / divisor : 0;
    const auto id = NewId();
    auto assets = Assets();
    assets.append(QJsonObject{{"id", id}, {"path", path}, {"name", QFileInfo(path).fileName()},
        {"kind", video.isEmpty() ? "audio" : "video"}, {"duration", duration},
        {"width", video.value("width").toInt()}, {"height", video.value("height").toInt()},
        {"fps", fps}, {"hasAudio", hasAudio}});
    auto project = Project();
    project.insert("assets", assets);
    SetProject(project);
    return id;
}

void RoomDocument::AddClip(const QString& assetId, double start)
{
    const auto asset = Asset(assetId);
    if (asset.isEmpty())
        return;
    auto project = Project();
    auto clips = Clips();
    const bool video = asset.value("kind") == QJsonValue("video");
    const bool audio = asset.value("hasAudio").toBool();
    const auto link = video && audio ? NewId() : QString();
    for (const auto& kind : {QStringLiteral("video"), QStringLiteral("audio")})
    {
        if ((kind == "video" && !video) || (kind == "audio" && !audio))
            continue;
        const auto key = kind + QStringLiteral("Tracks");
        auto tracks = project.value(key).toArray();
        if (tracks.isEmpty())
        {
            tracks.append(Track(kind));
            project.insert(key, tracks);
        }
        auto clip = NewClip(asset, tracks.first().toObject(), start);
        if (!link.isEmpty())
            clip.insert("linkID", link);
        clips.append(clip);
    }
    project.insert("clips", clips);
    SetProject(project);
}

bool RoomDocument::Split(qsizetype index, double time)
{
    auto clips = Clips();
    if (!InBounds(index, clips))
        return false;
    const auto selected = clips[index].toObject();
    const auto link = selected.value("linkID").toString();
    const auto nextLink = link.isEmpty() ? QString() : NewId();
    QJsonArray newClips;
    for (qsizetype i = 0; i < clips.size(); ++i)
    {
        auto clip = clips[i].toObject();
        if (i != index && (link.isEmpty() || clip.value("linkID") != link))
        {
            newClips.append(clip);
            continue;
        }
        const auto start = clip.value("start").toDouble();
        const auto duration = clip.value("duration").toDouble();
        const auto elapsed = time - start;
        if (HasAdvancedRendering(clip) || elapsed < 1.0 / 60 || duration - elapsed < 1.0 / 60)
            return false;
        auto right = clip;
        right.insert("id", NewId());
        right.insert("start", time);
        right.insert("sourceStart", clip.value("sourceStart").toDouble() + elapsed);
        right.insert("duration", duration - elapsed);
        if (!nextLink.isEmpty())
            right.insert("linkID", nextLink);
        clip.insert("duration", elapsed);
        newClips.append(clip);
        newClips.append(right);
    }
    auto project = Project();
    project.insert("clips", newClips);
    SetProject(project);
    return true;
}

bool RoomDocument::SetClipRange(qsizetype index, double start, double sourceStart, double duration)
{
    auto clips = Clips();
    if (!InBounds(index, clips) || !std::isfinite(start) || !std::isfinite(sourceStart) ||
        !std::isfinite(duration) || start < 0 || sourceStart < 0 || duration < 1.0 / 60)
        return false;
    const auto selected = clips[index].toObject();
    const auto link = selected.value("linkID").toString();
    for (qsizetype i = 0; i < clips.size(); ++i)
    {
        auto clip = clips[i].toObject();
        if (i != index && (link.isEmpty() || clip.value("linkID") != link))
            continue;
        const auto asset = Asset(clip.value("assetID").toString());
        if (HasAdvancedRendering(clip) || asset.isEmpty() ||
            sourceStart + duration > asset.value("duration").toDouble() + 1e-6)
            return false;
        clip.insert("start", start);
        clip.insert("sourceStart", sourceStart);
        clip.insert("duration", duration);
        clips.replace(i, clip);
    }
    auto project = Project();
    project.insert("clips", clips);
    SetProject(project);
    return true;
}

void RoomDocument::DeleteClip(qsizetype index)
{
    const auto clips = Clips();
    if (!InBounds(index, clips))
        return;
    const auto link = clips[index].toObject().value("linkID").toString();
    QJsonArray remaining;
    for (qsizetype i = 0; i < clips.size(); ++i)
        if (i != index && (link.isEmpty() || clips[i].toObject().value("linkID") != link))
            remaining.append(clips[i]);
    auto project = Project();
    project.insert("clips", remaining);
    SetProject(project);
}

QJsonObject RoomDocument::Asset(const QString& id) const
{
    for (const auto& value : Assets())
    {
        const auto asset = value.toObject();
        if (asset.value("id").toString() == id)
            return asset;
    }
    return {};
}

QString RoomDocument::MediaPath(const QJsonObject& asset) const
{
    const auto path = asset.value("path").toString();
    if (QFileInfo(path).isAbsolute() && QFileInfo(path).isFile())
        return path;
    const auto relative = asset.value("relativePath").toString();
    if (m_Path.isEmpty() || relative.isEmpty() || QFileInfo(relative).isAbsolute())
        return {};
    const QFileInfo moved(QFileInfo(m_Path).absoluteDir().absoluteFilePath(relative));
    return moved.isFile() ? moved.canonicalFilePath() : QString();
}

double RoomDocument::Duration() const
{
    double duration = 0;
    for (const auto& value : Clips())
    {
        const auto clip = value.toObject();
        duration = std::max(duration, clip.value("start").toDouble() + clip.value("duration").toDouble());
    }
    return duration;
}

QStringList RoomDocument::RenderArguments(const QString& output, const QString& manifest, QString& filters, QString& error) const
{
    const auto project = Project();
    const auto width = project.value("width").toInt(1920);
    const auto height = project.value("height").toInt(1080);
    const auto fps = project.value("fps").toDouble(30);
    const auto fullDuration = Duration();
    if (fullDuration <= 0 || width < 2 || height < 2 || width % 2 != 0 || height % 2 != 0 ||
        width > 16384 || height > 16384 || !std::isfinite(fps) || fps <= 0 || fps > 240)
    {
        error = QStringLiteral("Add media and choose valid video dimensions before rendering.");
        return {};
    }
    if (project.value("captions").isObject() || project.value("inPoint").isDouble() ||
        project.value("outPoint").isDouble())
    {
        error = QStringLiteral("This platform build cannot render this project's captions or export range.");
        return {};
    }

    QStringList arguments{"-hide_banner", "-nostdin", "-y"};
    QStringList graph{"color=c=black:s=" + QString::number(width) + 'x' + QString::number(height) +
                      ":r=" + Number(fps) + ":d=" + Number(fullDuration) + "[base]"};
    QString video = "[base]";
    QStringList audio;
    const auto clips = Clips();
    int input = 0;
    for (const auto& trackKind : {QStringLiteral("video"), QStringLiteral("audio")})
    {
        for (const auto& trackValue : project.value(trackKind + QStringLiteral("Tracks")).toArray())
        {
            const auto track = trackValue.toObject();
            if ((trackKind == "video" && track.value("hidden").toBool()) ||
                (trackKind == "audio" && track.value("muted").toBool()))
                continue;
            if (track.value("solo").toBool() || track.value("duckUnder").isString())
            {
                error = QStringLiteral("This platform build cannot render solo or ducked audio tracks.");
                return {};
            }
            for (const auto& clipValue : clips)
            {
                const auto clip = clipValue.toObject();
                if (clip.value("trackID") != track.value("id"))
                    continue;
                const auto kind = clip.value("kind").toString();
                if (kind != trackKind || HasAdvancedRendering(clip))
                {
                    error = QStringLiteral("This platform build cannot render one of the timeline's effects or clip types.");
                    return {};
                }
                const auto asset = Asset(clip.value("assetID").toString());
                const auto source = MediaPath(asset);
                if (source.isEmpty())
                {
                    error = QStringLiteral("Media is missing for %1.").arg(asset.value("name").toString());
                    return {};
                }
                const double start = clip.value("start").toDouble(-1);
                const double sourceStart = clip.value("sourceStart").toDouble(-1);
                const double duration = clip.value("duration").toDouble(-1);
                if (start < 0 || sourceStart < 0 || duration <= 0)
                {
                    error = QStringLiteral("The timeline contains an invalid clip range.");
                    return {};
                }
                const auto id = QString::number(input++);
                arguments << "-ss" << Number(sourceStart) << "-t" << Number(duration) << "-i" << source;
                if (kind == "video")
                {
                    const auto label = "[v" + id + ']';
                    const auto composition = "[mix" + id + ']';
                    graph << '[' + id + ":v]scale=" + QString::number(width) + ':' + QString::number(height) +
                        ":force_original_aspect_ratio=decrease,pad=" + QString::number(width) + ':' +
                        QString::number(height) + ":(ow-iw)/2:(oh-ih)/2,setsar=1,setpts=PTS-STARTPTS+" +
                        Number(start) + "/TB" + label;
                    graph << video + label + "overlay=eof_action=pass:enable='between(t," +
                        Number(start) + ',' + Number(start + duration) + ")'" + composition;
                    video = composition;
                }
                else
                {
                    const auto label = "[a" + id + ']';
                    const auto gain = clip.value("gainDB").toDouble() + track.value("gainDB").toDouble();
                    graph << '[' + id + ":a]asetpts=PTS-STARTPTS,aresample=48000,volume=" + Number(gain) +
                        "dB,adelay=" + QString::number(qRound64(start * 48000)) + "S:all=1" + label;
                    audio.push_back(label);
                }
            }
        }
    }

    arguments << "-/filter_complex" << manifest << "-map" << video;
    if (!audio.isEmpty())
    {
        graph << audio.join("") + "amix=inputs=" + QString::number(audio.size()) + ":normalize=0,apad[audio]";
        arguments << "-map" << "[audio]" << "-c:a" << "aac";
    }
    filters = graph.join(";\n");
    arguments << "-t" << Number(fullDuration) << "-c:v" << "libx264" << "-pix_fmt" << "yuv420p"
              << "-movflags" << "+faststart" << output;
    return arguments;
}
}
