#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace LuciaMedia
{
class RoomDocument
{
public:
    RoomDocument();

    bool Open(const QString& path, QString& error);
    bool Save(const QString& path, QString& error);
    QString Import(const QString& path, const QJsonObject& probe, QString& error);
    void AddClip(const QString& assetId, double start);
    bool Split(qsizetype index, double time);
    bool SetClipRange(qsizetype index, double start, double sourceStart, double duration);
    void DeleteClip(qsizetype index);

    QJsonArray Assets() const;
    QJsonArray Clips() const;
    QJsonObject Asset(const QString& id) const;
    QJsonArray MissingAssets() const;
    bool RelinkMissingAsset(const QString& id, const QString& path);
    QString MediaPath(const QJsonObject& asset) const;
    double Duration() const;
    QStringList RenderArguments(const QString& output, const QString& manifest, QString& filters, QString& error) const;

private:
    QJsonObject Project() const;
    void SetProject(QJsonObject project);
    QString m_Path;
    QJsonObject m_Document;
};
}
