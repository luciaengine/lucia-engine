#pragma once

#include "Rendering/Core/RecomputeElision.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Retain the union of masks for exact submitted raster inputs. A stable
// unjittered domain permits accumulation; an individual jitter phase is reused
// only after its complete input blob contributed to this physical allocation.
class VirtualShadowRequestReuse
{
public:
    struct Write
    {
        uint64_t Ticket = 0;
        bool Clear = true;
    };

    bool CanReuse(uint64_t frame, const Rendering::ElisionInputBlob& domain,
                  const Rendering::ElisionInputBlob& phase, uint64_t allocation,
                  bool needsInitialization, bool enabled)
    {
        m_CurrentFrame = frame;
        if (!m_HasDomain || needsInitialization || allocation != m_Allocation || domain != m_Domain)
        {
            ForgetContributions();
            m_Domain = domain;
            m_Allocation = allocation;
            m_HasDomain = true;
            m_Gate.ForceRecompute();
        }
        m_Enabled = enabled;
        const auto decision = m_Gate.Evaluate(frame, Rendering::ElisionInputBlob(domain), enabled, 3);
        if (decision.Cause == Rendering::ElisionCause::EvaluationGap || !enabled)
            ForgetContributions();
        if (decision.Skip && !m_Settled && m_ConsecutiveSubmitted >= 3)
        {
            // Warmup masks can precede producer convergence. Start the reusable
            // union with a new submitted mask after three submitted warmup frames.
            ForgetContributions();
            m_Settled = true;
        }
        return decision.Skip && m_Settled && m_Ready &&
            std::find(m_SubmittedPhases.begin(), m_SubmittedPhases.end(), phase) != m_SubmittedPhases.end();
    }

    Write PrepareWrite()
    {
        if (!m_HasDomain)
            return {};
        const bool clear = !m_Ready || !m_Enabled;
        if (clear)
            m_SubmittedPhases.clear();
        m_Ready = false;
        m_PreparedFrame = m_CurrentFrame;
        return {++m_Ticket, clear};
    }

    bool MarkSubmitted(uint64_t ticket, Rendering::ElisionInputBlob phase, uint64_t allocation)
    {
        if (!m_Enabled || ticket == 0 || ticket != m_Ticket || allocation != m_Allocation)
            return false;
        if (!m_HasSubmittedFrame || m_PreparedFrame != m_LastSubmittedFrame + 1)
            m_ConsecutiveSubmitted = 1;
        else if (m_ConsecutiveSubmitted < 3)
            ++m_ConsecutiveSubmitted;
        m_LastSubmittedFrame = m_PreparedFrame;
        m_HasSubmittedFrame = true;
        if (std::find(m_SubmittedPhases.begin(), m_SubmittedPhases.end(), phase) == m_SubmittedPhases.end())
        {
            // Unknown phases always execute. Eviction loses a skip opportunity,
            // never proof of pixels: the GPU union remains a conservative superset.
            if (m_SubmittedPhases.size() == kMaxSubmittedPhases)
                m_SubmittedPhases.erase(m_SubmittedPhases.begin());
            m_SubmittedPhases.push_back(std::move(phase));
        }
        m_Ready = true;
        return true;
    }

private:
    void ForgetContributions()
    {
        m_Ready = false;
        m_Settled = false;
        m_HasSubmittedFrame = false;
        m_ConsecutiveSubmitted = 0;
        m_SubmittedPhases.clear();
        ++m_Ticket; // An older graph must not restore proof after invalidation.
    }

    static constexpr size_t kMaxSubmittedPhases = 64;
    Rendering::RecomputeElisionGate m_Gate;
    Rendering::ElisionInputBlob m_Domain;
    std::vector<Rendering::ElisionInputBlob> m_SubmittedPhases;
    uint64_t m_Allocation = 0;
    uint64_t m_Ticket = 0;
    uint64_t m_CurrentFrame = 0;
    uint64_t m_PreparedFrame = 0;
    uint64_t m_LastSubmittedFrame = 0;
    uint32_t m_ConsecutiveSubmitted = 0;
    bool m_HasSubmittedFrame = false;
    bool m_Settled = false;
    bool m_HasDomain = false;
    bool m_Ready = false;
    bool m_Enabled = false;
};
}
