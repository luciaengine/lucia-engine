#include "ClaudeUserContent.h"

#include "IAgentProvider.h"
#include "Types/Base64.h"

#include <nlohmann/json.hpp>

namespace GameEngine
{
nlohmann::json ClaudeUserContent(const AgentTurnRequest& request)
{
    if (request.Images.empty())
        return request.UserText;
    nlohmann::json content = nlohmann::json::array();
    for (const PromptFile& image : request.Images)
        content.push_back({{"type", "image"},
                           {"source", {{"type", "base64"},
                                       {"media_type", image.MediaType},
                                       {"data", Base64Encode(image.Bytes)}}}});
    content.push_back({{"type", "text"}, {"text", request.UserText}});
    return content;
}
} // namespace GameEngine
