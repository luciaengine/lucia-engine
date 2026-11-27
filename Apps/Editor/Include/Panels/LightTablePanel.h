#pragma once

#include "Panels/WebPanel.h"

#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Editor
{
class LightTablePanel final : public WebPanel
{
public:
    LightTablePanel();

    /// The current browser tab and the local server must refer to the same instance.
    std::string ConnectedOrigin() const;

    /// Run on a worker. The request never supplies a URL or credential.
    static nlohmann::json RunDocumentCommand(const std::string& origin, const nlohmann::json& request);
};
}
