#include "Graph/GraphGlslAuthoring.h"

#include "Graph/SgGraphModelBridge.h"

#include "Rendering/ShaderGraph/SgTagParser.h"
#include "Types/StringUtils.h"

#include <sstream>
#include <string>

namespace GameEngine {
namespace Graph {
namespace {

std::string StripCommentPrefix(std::string_view line)
{
    std::string_view body = TrimWhitespaceView(line);
    if (body.starts_with("//"))
    {
        body.remove_prefix(2);
        if (!body.empty() && body.front() == ' ')
            body.remove_prefix(1);
    }
    return std::string(body);
}

} // namespace

std::optional<std::string> ExtractAuthoringJson(std::string_view comments)
{
    std::istringstream stream{std::string(comments)};
    std::string line;
    bool inFence = false;
    std::string json;
    while (std::getline(stream, line))
    {
        const std::string body = StripCommentPrefix(line);
        if (!inFence)
        {
            if (body == kGlslJsonFenceBegin)
                inFence = true;
            continue;
        }
        if (body == kGlslJsonFenceEnd)
            return json;
        json += body;
        json += '\n';
    }
    if (inFence)
        return std::string{};
    return std::nullopt;
}

std::string AppendAuthoringJsonFence(std::string tagBlock, const std::string& json)
{
    if (!tagBlock.empty() && tagBlock.back() != '\n')
        tagBlock += '\n';
    tagBlock += "// ";
    tagBlock += kGlslJsonFenceBegin;
    tagBlock += '\n';
    std::istringstream stream(json);
    std::string line;
    while (std::getline(stream, line))
    {
        tagBlock += "// ";
        tagBlock += line;
        tagBlock += '\n';
    }
    tagBlock += "// ";
    tagBlock += kGlslJsonFenceEnd;
    tagBlock += '\n';
    return tagBlock;
}

bool LoadModelFromShaderGraphComments(std::string_view tagBlock, Model& outModel)
{
    if (const std::optional<std::string> json = ExtractAuthoringJson(tagBlock))
    {
        if (!FromJson(*json, outModel))
            return false;
        outModel.KindId.assign(kKindIdMaterial);
        // The JSON fence is authoritative for the model, but unknown @sg-* tag
        // lines only live in the tag block; collect them so a save re-emits them.
        outModel.UnknownSgTags =
            ShaderGraph::ParseGraphDocumentFromTags(std::string(tagBlock)).UnknownTags;
        return true;
    }

    const ShaderGraph::SgGraphDocument document =
        ShaderGraph::ParseGraphDocumentFromTags(std::string(tagBlock));
    outModel = SgDocumentToGraphModel(document);
    outModel.KindId.assign(kKindIdMaterial);
    return true;
}

} // namespace Graph
} // namespace GameEngine
