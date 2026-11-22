#pragma once

#include <string_view>

namespace GameEngine::TensorFoldConnection
{
/// The TensorFold (Local) connection: a TensorFold server on this computer, through
/// OpenAiCompatibleProvider, with no key. TensorFold Bar serves at kDefaultBaseUrl as "local-model";
/// a server started without the Bar may use another address and lists its model by name, so the
/// URL is a setting (AiAssistantSettings::TensorFoldUrl) and a turn asks for the first model the
/// server lists.
inline constexpr std::string_view kId = "tensorfold-local";
inline constexpr std::string_view kDefaultBaseUrl = "http://127.0.0.1:8080/v1";
} // namespace GameEngine::TensorFoldConnection
