#pragma once

#include <memory>
#include <string_view>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
class CancelToken;

/// Reads the model list of a server connection (TensorFold, the custom endpoint) on a background
/// job and records it with AiAssistantSettings::SetServedModel(), so the panel names the served
/// model and offers its efforts before the first turn. A new Ask() or the probe's destruction
/// cancels the read in flight.
class ServedModelProbe
{
public:
    explicit ServedModelProbe(JobSystem::WorkStealingThreadPool& jobs);
    ~ServedModelProbe();
    ServedModelProbe(const ServedModelProbe&) = delete;
    ServedModelProbe& operator=(const ServedModelProbe&) = delete;

    /// Reads the list of `providerId`'s server; does nothing for any other connection.
    void Ask(std::string_view providerId);

private:
    JobSystem::WorkStealingThreadPool& m_Jobs;
    std::shared_ptr<CancelToken> m_Cancel;
};
} // namespace GameEngine
