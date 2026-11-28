#pragma once

#include "QtWorkspaceHost.h"

#include <QSize>

#include <algorithm>

namespace LuciaMedia
{
/// Both Qt hosts use the same request contract and the native project's undo history.
template<class Inspect, class Create, class Rename>
QJsonObject CompositorDocumentCommand(const QJsonObject& request, const QString& documentId,
                                      Inspect inspect, Create create, Rename rename)
{
    const auto operation = request.value("operation").toString();
    if (operation == "new_document")
    {
        if (!documentId.isEmpty())
            return DocumentError(QStringLiteral("Close the current canvas before creating another."));
        const QSize size(request.value("width").toInt(), request.value("height").toInt());
        if (size.isEmpty())
            return DocumentError(QStringLiteral("Provide a valid canvas width and height in pixels."));
        const auto created = create(size);
        if (created.isEmpty())
            return DocumentError(QStringLiteral("The canvas could not be created."));
        return {{"documentId", created}};
    }
    if (documentId.isEmpty())
        return DocumentError(QStringLiteral("Open a Compositor document first."));
    if (operation == "inspect")
    {
        auto result = inspect(std::max(0, request.value("offset").toInt()));
        result.insert("documentId", documentId);
        return result;
    }
    if (request.value("documentId").toString() != documentId)
        return DocumentError(QStringLiteral("The Compositor document changed. Inspect it again."));
    const auto id = request.value("itemId").toString();
    const auto name = request.value("name").toString().trimmed();
    if (operation != "rename_layer" || id.isEmpty() || name.isEmpty() || name.size() > 256)
        return DocumentError(QStringLiteral("Provide an existing Compositor layer id and a new name."));
    if (!rename(id, name))
        return DocumentError(QStringLiteral("The layer is unavailable or Compositor is busy."));
    return {{"documentId", documentId}, {"itemId", id}, {"name", name}};
}
}
