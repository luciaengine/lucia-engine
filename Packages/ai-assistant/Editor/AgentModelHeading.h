#pragma once

#include <cstdint>
#include <string>

namespace GameEngine
{
class Label;

/// The panel's top-bar line that names what the next prompt talks to: the assistant, its model
/// and its effort ("TensorFold · inkling-2-mini · High"). It follows the connection and every
/// change of AiAssistantSettings::ModelGeneration().
class AgentModelHeading
{
public:
    explicit AgentModelHeading(Label& label);

    /// Shows `providerId`'s line when the connection or a model or effort changed since the last call.
    void Update(const std::string& providerId);

    /// The line for `providerId`.
    static std::string Text(const std::string& providerId);

private:
    Label& m_Label;
    std::string m_ProviderId;
    uint64_t m_Generation = UINT64_MAX;
};
} // namespace GameEngine
