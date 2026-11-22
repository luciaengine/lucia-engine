#include "AgentModelHeading.h"

#include "AiAssistantSettings.h"

#include "UI/Controls/Label.h"

namespace GameEngine
{
AgentModelHeading::AgentModelHeading(Label& label) : m_Label(label)
{
}

void AgentModelHeading::Update(const std::string& providerId)
{
    const uint64_t generation = AiAssistantSettings::ModelGeneration();
    if (providerId == m_ProviderId && generation == m_Generation)
        return;
    m_ProviderId = providerId;
    m_Generation = generation;
    const std::string text = Text(providerId);
    m_Label.SetText(text);
    m_Label.SetTooltip(std::string(AiAssistantSettings::ProviderChoiceOf(providerId).Label) + "\n" + text);
}

std::string AgentModelHeading::Text(const std::string& providerId)
{
    std::string text(AiAssistantSettings::ProviderChoiceOf(providerId).Assistant);
    std::string model = AiAssistantSettings::Model(providerId);
    if (model.empty())
        model = AiAssistantSettings::ServedModelName(providerId);
    if (!model.empty())
        text += " · " + AiAssistantSettings::ModelLabel(providerId, model);
    const std::string effort = AiAssistantSettings::Effort(providerId);
    for (const AiAssistantSettings::Choice& choice : AiAssistantSettings::EffortChoices(providerId))
        if (!effort.empty() && choice.Id == effort)
            text += " · " + std::string(choice.Label);
    return text;
}
} // namespace GameEngine
