#include "Animation/Nodes/ClipPlayerNode.h"

#include "Animation/AnimationEvent.h"
#include "Animation/Compression/AnimationClipData.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Assets/AnimationClip.h"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace GameEngine
{
namespace Animation
{

namespace
{

void SampleChannel(const AnimationClipData& data, const AnimationClipData::Track& track,
    float time, AnimationPose& pose)
{
    if (track.BoneIndex >= pose.BoneCount || track.Path == AnimPath::MorphWeight)
        return;
    Mathematics::Vector4 value;
    if (!data.Sample(track, time, value))
        return;
    if (track.Path == AnimPath::Translation)
        pose.Positions[track.BoneIndex] = Mathematics::Vector3(value.x, value.y, value.z);
    else if (track.Path == AnimPath::Scale)
        pose.Scales[track.BoneIndex] = Mathematics::Vector3(value.x, value.y, value.z);
    else
        pose.Rotations[track.BoneIndex] = Mathematics::Quaternion(value.w, value.x, value.y, value.z);
}

} // anonymous namespace

void ClipPlayerNode::SetClip(std::shared_ptr<AnimationClip> clip)
{
    m_Clip = std::move(clip);
    m_Time = 0.0f;
    if (m_Clip)
        m_ClipGuid = m_Clip->GetGUID();
}

void ClipPlayerNode::SetClipGuid(const GUID& guid)
{
    m_ClipGuid = guid;
}

const GUID& ClipPlayerNode::GetClipGuid() const
{
    return m_ClipGuid;
}

void ClipPlayerNode::SetSpeed(float speed)
{
    m_Speed = std::isfinite(speed) ? speed : 1.0f;
}

void ClipPlayerNode::SetLooping(bool loop)
{
    m_Loop = loop;
}

void ClipPlayerNode::SetTime(float time)
{
    m_Time = time;
}

float ClipPlayerNode::GetTime() const
{
    return m_Time;
}

float ClipPlayerNode::GetDuration() const
{
    if (!m_Clip)
        return 0.0f;
    return m_Clip->GetDuration();
}

float ClipPlayerNode::GetNormalizedTime() const
{
    float duration = GetDuration();
    if (duration <= 0.0f)
        return 0.0f;
    return m_Time / duration;
}

float ClipPlayerNode::GetSpeed() const { return m_Speed; }
bool ClipPlayerNode::GetLooping() const { return m_Loop; }
const std::shared_ptr<AnimationClip>& ClipPlayerNode::GetClip() const { return m_Clip; }

void ClipPlayerNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (!m_Clip)
    {
        outPose.Resize(0);
        return;
    }

    EvaluateStep(ctx, outPose, Advance(ctx.DeltaTime));
}

PlaybackStep ClipPlayerNode::Advance(float deltaTime) const
{
    // Advance playback time. Non-finite speed/dt would livelock a
    // subtract-while wrap; fmod is O(1) for huge finite values too.
    const float previousTime = m_Time;
    float dt = std::max(0.0f, deltaTime) * std::max(0.0f, m_Speed);
    if (!std::isfinite(dt))
        dt = 0.0f;
    float nextTime = m_Time + dt;
    if (!std::isfinite(nextTime))
        nextTime = 0.0f;

    float duration = m_Clip->GetDuration();
    PlaybackStepKind stepKind = PlaybackStepKind::Forward;
    if (duration > 0.0f)
    {
        if (m_Loop)
        {
            // A step that did not move crosses nothing, even sitting on the lap's end.
            if (nextTime >= duration && nextTime != previousTime)
                stepKind = nextTime - std::max(previousTime, 0.0f) >= duration ? PlaybackStepKind::WholeLap
                                                                             : PlaybackStepKind::Wrapped;
            nextTime = std::fmod(nextTime, duration);
            if (nextTime < 0.0f)
                nextTime += duration;
        }
        else if (nextTime > duration)
        {
            nextTime = duration;
        }
    }

    return {previousTime, nextTime, 0.0f, duration, stepKind};
}

void ClipPlayerNode::EvaluateStep(EvaluationContext& ctx, AnimationPose& outPose, const PlaybackStep& step)
{
    m_Time = step.To;
    if (ctx.Events && ctx.Weight > 0.0f)
        CollectCrossedEvents(m_Clip->GetEventTrack(), step, *ctx.Events);

    const auto data = m_Clip->GetRuntimeData();
    if (!data)
    {
        outPose.Resize(0u);
        return;
    }
    // Size the pose from the immutable channel metadata.
    uint32_t maxBone = 0;
    for (const auto& channel : data->Tracks())
    {
        if (channel.BoneIndex >= maxBone)
            maxBone = channel.BoneIndex + 1;
    }

    outPose.Resize(maxBone);

    // Sample each channel into the pose
    for (const auto& channel : data->Tracks())
        SampleChannel(*data, channel, m_Time, outPose);
}

} // namespace Animation
} // namespace GameEngine
