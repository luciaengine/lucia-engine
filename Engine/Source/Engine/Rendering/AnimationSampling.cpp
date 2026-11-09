#include "Engine/Rendering/AnimationSampling.h"

#include "Animation/AnimationEvent.h"
#include "Animation/Compression/AnimationClipData.h"
#include "Animation/BoneEvaluationPlan.h"
#include "Animation/PoseToSkinMatrices.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace GameEngine
{
namespace Engine::Renderer
{

double WrapClipTime(double time, float32 lapStart, float32 lapEnd)
{
    const double lapDuration = static_cast<double>(lapEnd) - lapStart;
    if (!std::isfinite(time) || !std::isfinite(lapDuration) || lapDuration <= 0.0)
        return lapStart;
    if (time < lapEnd)
        return time;
    return lapStart + std::fmod(time - lapStart, lapDuration);
}

namespace
{

glm::mat4 BuildLocalTRS(const glm::vec3& t, const glm::quat& r, const glm::vec3& s)
{
    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

void StoreColumnMajor(const glm::mat4& matrix, float* out)
{
    std::memcpy(out, glm::value_ptr(matrix), 16 * sizeof(float));
}

// Initialize the workspace pose to the skeleton's rest TRS and reset the
// Changed* arrays. Shared between the regular and retargeted sample paths.
void InitPoseToRest(const SkeletonData& skeleton, PoseSampleWorkspace& ws)
{
    const uint32 bones = skeleton.BoneCount;
    ws.Pose.clear();
    ws.Pose.resize(bones);
    if (skeleton.RestTranslation.size() == bones * 3u &&
        skeleton.RestRotation.size() == bones * 4u &&
        skeleton.RestScale.size() == bones * 3u)
    {
        for (uint32 i = 0; i < bones; ++i)
        {
            ws.Pose[i].t = glm::vec3(skeleton.RestTranslation[i * 3 + 0],
                                     skeleton.RestTranslation[i * 3 + 1],
                                     skeleton.RestTranslation[i * 3 + 2]);
            ws.Pose[i].r = glm::quat(skeleton.RestRotation[i * 4 + 3],
                                     skeleton.RestRotation[i * 4 + 0],
                                     skeleton.RestRotation[i * 4 + 1],
                                     skeleton.RestRotation[i * 4 + 2]);
            ws.Pose[i].s = glm::vec3(skeleton.RestScale[i * 3 + 0],
                                     skeleton.RestScale[i * 3 + 1],
                                     skeleton.RestScale[i * 3 + 2]);
        }
    }
    ws.ChangedTranslation.assign(bones, 0u);
    ws.ChangedRotation.assign(bones, 0u);
    ws.ChangedScale.assign(bones, 0u);
}

// BuildPoseToSkinMatrices was moved to GameEngine::Animation in Phase 0c.

} // namespace {

void SampleAnimationPose(const SkeletonData& skeleton,
                         const AnimationClip* clip,
                         float time,
                         std::vector<float>* outNodeWorldMatrices,
                         std::vector<float>* outCompactSkinMatrices,
                         PoseSampleWorkspace& workspace,
                         const Animation::BoneEvaluationPlan* plan)
{
    const uint32 bones = skeleton.BoneCount;
    const bool reduced = plan && plan->IsForSkeleton(skeleton);
    if (bones == 0)
    {
        if (outNodeWorldMatrices)
            outNodeWorldMatrices->clear();
        if (outCompactSkinMatrices)
            outCompactSkinMatrices->clear();
        return;
    }

    // Prepare workspace vectors (clear + resize retains capacity after first call).
    workspace.Pose.clear();
    workspace.Pose.resize(bones);
    if (skeleton.RestTranslation.size() == bones * 3u &&
        skeleton.RestRotation.size() == bones * 4u &&
        skeleton.RestScale.size() == bones * 3u)
    {
        for (uint32 index = 0; index < bones; ++index)
        {
            workspace.Pose[index].t = glm::vec3(skeleton.RestTranslation[index * 3 + 0],
                                     skeleton.RestTranslation[index * 3 + 1],
                                     skeleton.RestTranslation[index * 3 + 2]);
            workspace.Pose[index].r = glm::quat(skeleton.RestRotation[index * 4 + 3],
                                     skeleton.RestRotation[index * 4 + 0],
                                     skeleton.RestRotation[index * 4 + 1],
                                     skeleton.RestRotation[index * 4 + 2]);
            workspace.Pose[index].s = glm::vec3(skeleton.RestScale[index * 3 + 0],
                                     skeleton.RestScale[index * 3 + 1],
                                     skeleton.RestScale[index * 3 + 2]);
        }
    }

    workspace.ChangedTranslation.clear();
    workspace.ChangedTranslation.resize(bones, 0u);
    workspace.ChangedRotation.clear();
    workspace.ChangedRotation.resize(bones, 0u);
    workspace.ChangedScale.clear();
    workspace.ChangedScale.resize(bones, 0u);

    if (clip)
    {
        const auto data = clip->GetRuntimeData();
        const auto channels = data ? data->Tracks() : std::span<const Animation::AnimationClipData::Track>{};

        // Resolution policy:
        //   - If both clip channel and skeleton carry name hashes (StringId),
        //     look up the channel's targetNameId in the skeleton's pre-built
        //     hashmap and write to that bone.
        //   - If a name id is set but the skeleton can't resolve it, drop
        //     the channel. The legacy `channel.BoneIndex` is the source-
        //     skeleton's index and would write to whatever unrelated bone
        //     happens to live at that slot in the destination skeleton —
        //     actively corrupting the pose. Skipping leaves the bone at its
        //     rest transform, which is correct.
        //   - Otherwise (legacy clips with no name id, or skeletons with no
        //     name table — e.g. unit tests), trust the stored boneIndex.
        const bool skeletonHasNames = !skeleton.BoneNameLookup.empty();
        uint32 resolvedByName = 0;
        uint32 dropped = 0;
        std::vector<std::string> unmatchedNames;
        for (const auto& channel : channels)
        {
            if (channel.Path == AnimPath::MorphWeight)
                continue;
            uint32 resolvedIndex;
            if (channel.TargetNameId != 0 && skeletonHasNames)
            {
                resolvedIndex = skeleton.ResolveBoneIndex(channel.TargetNameId, UINT32_MAX);
                if (resolvedIndex == UINT32_MAX)
                {
                    ++dropped;
                    if (!channel.TargetName.empty()
                        && std::find(unmatchedNames.begin(), unmatchedNames.end(), channel.TargetName) == unmatchedNames.end())
                        unmatchedNames.push_back(channel.TargetName);
                    continue; // drop — see policy comment above
                }
                if (resolvedIndex != channel.BoneIndex)
                    ++resolvedByName;
            }
            else
            {
                resolvedIndex = channel.BoneIndex;
            }
            if (resolvedIndex >= bones || (reduced && plan->EvaluatedAncestors()[resolvedIndex] != resolvedIndex))
                continue;
            Mathematics::Vector4 value;
            if (!data->Sample(channel, time, value))
                continue;
            if (channel.Path == AnimPath::Translation)
            {
                workspace.Pose[resolvedIndex].t = glm::vec3(value.x, value.y, value.z);
                workspace.ChangedTranslation[resolvedIndex] = 1u;
            }
            else if (channel.Path == AnimPath::Scale)
            {
                workspace.Pose[resolvedIndex].s = glm::vec3(value.x, value.y, value.z);
                workspace.ChangedScale[resolvedIndex] = 1u;
            }
            else
            {
                workspace.Pose[resolvedIndex].r = glm::quat(value.w, value.x, value.y, value.z);
                workspace.ChangedRotation[resolvedIndex] = 1u;
            }
        }

        // One-shot diagnostic per (skeletonBoneCount, channelCount) pair.
        static std::unordered_map<uint64_t, bool> s_LoggedPairs;
        const uint64_t pairKey = (static_cast<uint64_t>(bones) << 32) | static_cast<uint64_t>(channels.size());
        if (s_LoggedPairs.find(pairKey) == s_LoggedPairs.end())
        {
            s_LoggedPairs[pairKey] = true;
            Logger::Log::Info("AnimationSampling: skeletonBones={} clipChannels={} | resolvedByName={} dropped={}",
                              bones, channels.size(), resolvedByName, dropped);
            std::string skeletonHead;
            for (uint32 index = 0; index < std::min<uint32>(bones, 6u); ++index)
            {
                if (index < skeleton.BoneNames.size() && !skeleton.BoneNames[index].empty())
                {
                    if (!skeletonHead.empty()) skeletonHead += ", ";
                    skeletonHead += skeleton.BoneNames[index];
                }
            }
            std::string clipHead;
            for (size_t index = 0; index < std::min<size_t>(channels.size(), 6u); ++index)
            {
                if (!channels[index].TargetName.empty())
                {
                    if (!clipHead.empty()) clipHead += ", ";
                    clipHead += channels[index].TargetName;
                }
            }
            Logger::Log::Info("AnimationSampling: skel head=[{}] | clip head=[{}]", skeletonHead, clipHead);
            if (!unmatchedNames.empty())
            {
                std::string unmatched;
                for (size_t index = 0; index < std::min<size_t>(unmatchedNames.size(), 30u); ++index)
                {
                    if (!unmatched.empty()) unmatched += ", ";
                    unmatched += unmatchedNames[index];
                }
                Logger::Log::Info("AnimationSampling: unmatched clip bones (first 30 of {}): [{}]",
                                  unmatchedNames.size(), unmatched);
            }
        }
    }

    ::GameEngine::Animation::BuildPoseToSkinMatrices(skeleton, outNodeWorldMatrices, outCompactSkinMatrices, workspace, plan);
}

namespace
{

// The kind of a looping step from `from` that reached `unwrappedTo` before any wrap: Forward when it stayed short of
// `lapEnd` or did not move (a speed of 0 on the lap's end crosses nothing), WholeLap when it travelled a whole lap or
// more from where it entered the lap, Wrapped otherwise.
Animation::PlaybackStepKind LoopingStepKind(float from, double unwrappedTo, float lapStart, float lapEnd)
{
    if (unwrappedTo < lapEnd || unwrappedTo == from)
        return Animation::PlaybackStepKind::Forward;
    if (unwrappedTo - std::max(from, lapStart) >= lapEnd - lapStart)
        return Animation::PlaybackStepKind::WholeLap;
    return Animation::PlaybackStepKind::Wrapped;
}

// Advances `time` by `stepSeconds` over `clip` and returns the step it made. The lap is the section when one plays, else the
// whole clip; a clip of no length has a lap of no length.
Animation::PlaybackStep AdvanceClipTime(float32& time,
                                        const AnimationClip& clip,
                                        double stepSeconds,
                                        bool loop,
                                        bool section,
                                        float32 sectionStart,
                                        float32 sectionEnd)
{
    Animation::PlaybackStep step;
    if (!std::isfinite(time))
        time = 0.0f;
    step.From = time;
    // Keep the unwrapped sum in double: finite float speeds can overflow a float step,
    // and a large step must not discard the starting fraction before the modulo.
    double advancedTime = static_cast<double>(time) + stepSeconds;
    const float duration = clip.GetDuration();
    if (!std::isfinite(duration) || duration <= 0.0f)
    {
        time = static_cast<float>(std::min(advancedTime, static_cast<double>(std::numeric_limits<float>::max())));
        step.To = time;
        return step;
    }
    if (section && sectionEnd > sectionStart)
    {
        const float start = std::clamp(sectionStart, 0.0f, duration);
        const float end = std::clamp(sectionEnd, start, duration);
        step.LapStart = start;
        step.LapEnd = end;
        advancedTime = std::max(advancedTime, static_cast<double>(start));
        if (loop)
        {
            const float sectionDuration = std::max(0.0f, end - start);
            if (sectionDuration > 0.0f)
                step.Kind = LoopingStepKind(step.From, advancedTime, start, end);
            if (sectionDuration > 0.0f)
                advancedTime = WrapClipTime(advancedTime, start, end);
        }
        else if (advancedTime > end)
        {
            advancedTime = end;
        }
        time = static_cast<float>(advancedTime);
        step.To = time;
        return step;
    }
    step.LapEnd = duration;
    if (loop)
    {
        step.Kind = LoopingStepKind(step.From, advancedTime, 0.0f, duration);
        advancedTime = WrapClipTime(advancedTime, 0.0f, duration);
    }
    else if (advancedTime > duration)
    {
        advancedTime = duration;
    }
    time = static_cast<float>(advancedTime);
    step.To = time;
    return step;
}

} // namespace

void TickAnimatorRef(Components::AnimatorRef& anim, ClipStore& clips, float32 deltaTime,
                     Animation::AnimationEventCollector* events)
{
    if (anim.IsPaused())
        return;

    const float frameSeconds = std::isfinite(deltaTime) ? std::max(0.0f, deltaTime) : 0.0f;
    const double clipStepSeconds = std::isfinite(anim.Speed)
        ? static_cast<double>(frameSeconds) * std::max(0.0f, anim.Speed) : 0.0;
    const SharedPtr<AnimationClip> clip = clips.Get(anim.ClipIndex);
    Animation::PlaybackStep incomingStep;
    if (clip)
    {
        incomingStep = AdvanceClipTime(anim.Time,
                                       *clip,
                                       clipStepSeconds,
                                       anim.IsLooping(),
                                       anim.IsSectionPlayback(),
                                       anim.SectionStart,
                                       anim.SectionEnd);
    }

    SharedPtr<AnimationClip> previous;
    Animation::PlaybackStep outgoingStep;
    if (anim.IsBlending())
    {
        // Wall-clock: blendSeconds on SetAnimation is real time.
        anim.BlendTime += frameSeconds;
        previous = clips.Get(anim.PrevClipIndex);
        if (previous)
        {
            outgoingStep = AdvanceClipTime(anim.PrevTime,
                                           *previous,
                                           clipStepSeconds,
                                           anim.IsLooping(),
                                           false,
                                           0.0f,
                                           0.0f);
        }
        if (!anim.IsBlending())
            anim.ClearBlend();
    }

    if (!events)
        return;
    // A clip fires while its weight in the frame's pose is above 0: the outgoing clip until the fade ends, the
    // incoming one from the fade's first frame.
    if (previous && anim.IsBlending())
        Animation::CollectCrossedEvents(previous->GetEventTrack(), outgoingStep, *events);
    if (clip && anim.BlendAlpha() > 0.0f)
        Animation::CollectCrossedEvents(clip->GetEventTrack(), incomingStep, *events);
}

} // namespace Engine::Renderer
} // namespace GameEngine
