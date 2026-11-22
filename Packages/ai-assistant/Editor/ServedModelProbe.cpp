#include "ServedModelProbe.h"

#include "AgentSessionState.h"
#include "AiAssistantSettings.h"
#include "Providers/CancelToken.h"
#include "Providers/OpenAiCompatibleProvider.h"

#include "JobSystem/WorkStealingThreadPool.h"

#include <string>
#include <utility>

namespace GameEngine
{
ServedModelProbe::ServedModelProbe(JobSystem::WorkStealingThreadPool& jobs) : m_Jobs(jobs)
{
}

ServedModelProbe::~ServedModelProbe()
{
    if (m_Cancel)
        m_Cancel->Cancel();
}

void ServedModelProbe::Ask(std::string_view providerId)
{
    const std::shared_ptr<OpenAiCompatibleProvider> provider =
        std::dynamic_pointer_cast<OpenAiCompatibleProvider>(AgentSessionState::Get().Provider(providerId));
    if (!provider)
        return;
    if (m_Cancel)
        m_Cancel->Cancel();
    m_Cancel = std::make_shared<CancelToken>();
    m_Jobs.EnqueueWork(
        [provider, cancel = m_Cancel, id = std::string(providerId)]()
        {
            OpenAiCompatibleProvider::ServedModel served = provider->ReadServedModel(*cancel);
            // A server that did not answer keeps what it said last.
            if (!cancel->IsCancelled() && !served.Name.empty())
                AiAssistantSettings::SetServedModel(id, served);
        },
        JobSystem::JobPriority::Background);
}
} // namespace GameEngine
