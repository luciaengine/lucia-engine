#include "Animation/Nodes/BlendSpace1DNode.h"

#include "Animation/AnimationClip.h"
#include "Animation/EvaluationContext.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "Types/StringId.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace GameEngine::Animation
{

namespace
{
bool CanSynchronize(const ClipPlayerNode* player)
{
    if (!player || !player->GetLooping() || !player->GetClip() ||
        !std::isfinite(player->GetDuration()) || !std::isfinite(player->GetTime()))
        return false;

    const auto& markers = player->GetClip()->GetSettings().SyncMarkers;
    return markers.size() >= 2 && markers.back().Time < player->GetDuration();
}

bool HasMatchingMarkers(const ClipPlayerNode* from, const ClipPlayerNode* to)
{
    if (!CanSynchronize(from) || !CanSynchronize(to))
        return false;
    const auto& source = from->GetClip()->GetSettings().SyncMarkers;
    const auto& target = to->GetClip()->GetSettings().SyncMarkers;
    return std::ranges::equal(source, target, {}, &AnimationSyncMarker::Name, &AnimationSyncMarker::Name);
}

double MapSyncTime(const ClipPlayerNode& from, const ClipPlayerNode& to, double time)
{
    const auto& source = from.GetClip()->GetSettings().SyncMarkers;
    const auto& target = to.GetClip()->GetSettings().SyncMarkers;
    const double sourceDuration = from.GetDuration();
    const double targetDuration = to.GetDuration();
    const double lap = std::floor(time / sourceDuration);
    const double localTime = time - lap * sourceDuration;
    const auto next = std::upper_bound(source.begin(), source.end(), localTime,
        [](double local, const AnimationSyncMarker& marker) { return local < marker.Time; });
    const size_t right = next == source.end() ? 0 : static_cast<size_t>(next - source.begin());
    const size_t left = right == 0 ? source.size() - 1 : right - 1;
    const double sourceStart = source[left].Time - (next == source.begin() ? sourceDuration : 0.0);
    const double sourceEnd = source[right].Time + (next == source.end() ? sourceDuration : 0.0);
    const double targetStart = target[left].Time - (next == source.begin() ? targetDuration : 0.0);
    const double targetEnd = target[right].Time + (next == source.end() ? targetDuration : 0.0);
    return std::lerp(targetStart, targetEnd, (localTime - sourceStart) / (sourceEnd - sourceStart)) +
           lap * targetDuration;
}

float WrapSyncTime(double time, float duration)
{
    const double wrapped = std::fmod(time, static_cast<double>(duration));
    const float result = static_cast<float>(wrapped < 0.0 ? wrapped + duration : wrapped);
    return result < duration ? result : std::nextafter(duration, 0.0f);
}
} // namespace

void BlendSpace1DNode::AddSample(std::unique_ptr<AnimGraphNode> node, float position)
{
    m_Samples.push_back({std::move(node), position});
}

void BlendSpace1DNode::Sort()
{
    std::sort(m_Samples.begin(), m_Samples.end(),
              [](const BlendSpace1DSample& a, const BlendSpace1DSample& b)
              { return a.Position < b.Position; });
}

void BlendSpace1DNode::SetParameterName(std::string_view name)
{
    m_ParameterName.assign(name);
    m_ParameterId = HashStringId(name);
}

void BlendSpace1DNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (m_ParameterId != 0 && ctx.Parameters)
    {
        auto it = ctx.Parameters->find(m_ParameterId);
        if (it != ctx.Parameters->end())
            m_Parameter = ParamAsFloat(it->second.Value);
    }

    if (m_Samples.empty())
    {
        return;
    }

    if (m_Samples.size() == 1)
    {
        if (m_Samples[0].Node)
            EvaluateSamples(ctx, outPose, *m_Samples[0].Node, nullptr, 0.0f);
        else
            outPose.Resize(0);
        return;
    }

    float clampedParam = std::clamp(m_Parameter, m_Samples.front().Position,
                                    m_Samples.back().Position);

    // Find the two bracketing samples via linear scan (sample counts are small)
    size_t upperIdx = 0;
    for (size_t i = 0; i < m_Samples.size(); ++i)
    {
        if (m_Samples[i].Position >= clampedParam)
        {
            upperIdx = i;
            break;
        }
    }

    // Exact match or at/below the first sample
    if (upperIdx == 0 || m_Samples[upperIdx].Position == clampedParam)
    {
        if (m_Samples[upperIdx].Node)
            EvaluateSamples(ctx, outPose, *m_Samples[upperIdx].Node, nullptr, 0.0f);
        else
            outPose.Resize(0);
        return;
    }

    size_t lowerIdx = upperIdx - 1;
    const auto& lower = m_Samples[lowerIdx];
    const auto& upper = m_Samples[upperIdx];

    float range = upper.Position - lower.Position;
    float alpha = (clampedParam - lower.Position) / range;

    if (!lower.Node || !upper.Node)
    {
        if (lower.Node)
            EvaluateSamples(ctx, outPose, *lower.Node, nullptr, 0.0f);
        else if (upper.Node)
            EvaluateSamples(ctx, outPose, *upper.Node, nullptr, 0.0f);
        else
            outPose.Resize(0);
        return;
    }
    EvaluateSamples(ctx, outPose, *lower.Node, upper.Node.get(), alpha);
}

void BlendSpace1DNode::EvaluateSamples(EvaluationContext& ctx, AnimationPose& outPose, AnimGraphNode& lower,
                                      AnimGraphNode* upper, float alpha)
{
    auto* leader = dynamic_cast<ClipPlayerNode*>(&lower);
    auto* follower = dynamic_cast<ClipPlayerNode*>(upper);
    const bool synchronize = upper ? HasMatchingMarkers(leader, follower) : CanSynchronize(leader);
    if (synchronize && m_SyncLeader && m_SyncLeader != leader && HasMatchingMarkers(m_SyncLeader, leader))
    {
        leader->SetTime(WrapSyncTime(MapSyncTime(*m_SyncLeader, *leader, m_SyncLeader->GetTime()),
                                     leader->GetDuration()));
    }
    m_SyncLeader = synchronize ? leader : nullptr;
    if (!upper)
    {
        lower.Evaluate(ctx, outPose);
        return;
    }
    if (synchronize)
    {
        EvaluationContext leaderContext = ctx;
        leaderContext.Weight *= 1.0f - alpha;
        const PlaybackStep step = leader->Advance(ctx.DeltaTime);
        leader->EvaluateStep(leaderContext, outPose, step);

        const float duration = follower->GetDuration();
        const double from = MapSyncTime(*leader, *follower, step.From);
        const double to = MapSyncTime(*leader, *follower, step.To) +
                          (step.Kind == PlaybackStepKind::Wrapped ? duration : 0.0);
        PlaybackStepKind kind = step.Kind;
        if (kind != PlaybackStepKind::WholeLap)
        {
            kind = std::floor(to / duration) > std::floor(from / duration) ?
                PlaybackStepKind::Wrapped : PlaybackStepKind::Forward;
        }
        EvaluationContext followerContext = ctx;
        followerContext.Weight *= alpha;
        follower->EvaluateStep(followerContext, m_TempPose,
                               {WrapSyncTime(from, duration), WrapSyncTime(to, duration), 0.0f, duration, kind});
    }
    else
    {
        EvaluateAtWeight(lower, ctx, 1.0f - alpha, outPose);
        EvaluateAtWeight(*upper, ctx, alpha, m_TempPose);
    }
    AnimationPose::Blend(outPose, m_TempPose, alpha, outPose);
}

} // namespace GameEngine::Animation
