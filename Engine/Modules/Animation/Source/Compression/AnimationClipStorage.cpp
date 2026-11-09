#include "Animation/AnimationClip.h"

#include "Animation/Compression/AnimationClipData.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <variant>

namespace GameEngine::Animation
{

void AnimationClip::SetDuration(float32 duration)
{
    if (!std::isfinite(duration))
        return;
    GetChannels();
    m_Duration = std::max(0.0f, duration);
    RebuildRuntimeData();
}

SharedPtr<const AnimationClipData> AnimationClip::GetRuntimeData() const
{
    return std::atomic_load_explicit(&m_RuntimeData, std::memory_order_acquire);
}

const std::vector<AnimChannel>& AnimationClip::GetChannels() const
{
    std::lock_guard lock(m_AuthoringMutex);
    const auto runtime = GetRuntimeData();
    if (m_Channels.empty() && runtime && !runtime->Tracks().empty())
    {
        if (!runtime->DecodeChannels(m_Channels))
            Logger::Log::Error("AnimationClip: could not restore authoring keys for '{}'.", GetPath().string());
    }
    return m_Channels;
}

size_t AnimationClip::GetMemoryUsage() const
{
    std::lock_guard lock(m_AuthoringMutex);
    const auto runtime = GetRuntimeData();
    size_t bytes = sizeof(*this) + m_CachedFileData.capacity() + m_Channels.capacity() * sizeof(AnimChannel);
    bytes += GetPath().native().capacity() * sizeof(std::filesystem::path::value_type) +
        m_SourcePath.native().capacity() * sizeof(std::filesystem::path::value_type) + GetTypeIdString().capacity();
    bytes += m_Settings.SyncMarkers.capacity() * sizeof(AnimationSyncMarker);
    for (const auto& marker : m_Settings.SyncMarkers)
        bytes += marker.Name.capacity();
    if (m_Settings.RootMotion)
        bytes += m_Settings.RootMotion->Bone.capacity();
    const auto& events = m_EventTrack.GetEvents();
    bytes += events.capacity() * sizeof(AnimationEvent);
    for (const auto& event : events)
    {
        bytes += event.Name.capacity();
        if (const auto* text = std::get_if<std::string>(&event.Data))
            bytes += text->capacity();
    }
    if (runtime)
        bytes += runtime->GetMemoryUsage();
    for (const auto& channel : m_Channels)
        bytes += channel.targetName.capacity() + channel.keys.capacity() * sizeof(AnimKeyframe);
    return bytes;
}

void AnimationClip::AdoptLoadedData(AnimationClip& loaded, SharedPtr<const AnimationClipData> runtime)
{
    std::lock_guard lock(m_AuthoringMutex);
    m_Duration = loaded.m_Duration;
    m_PublishedDuration = m_Duration;
    m_SelectedAnimation = loaded.m_SelectedAnimation;
    m_SourcePath = std::move(loaded.m_SourcePath);
    m_SourceAnimationIndex = loaded.m_SourceAnimationIndex;
    m_Settings = std::move(loaded.m_Settings);
    m_EventTrack = std::move(loaded.m_EventTrack);
    std::vector<AnimChannel>().swap(m_Channels);
    std::atomic_store_explicit(&m_RuntimeData, std::move(runtime), std::memory_order_release);
    SetState(AssetState::Loaded);
}

bool AnimationClip::RebuildRuntimeData()
{
    auto runtime = AnimationClipData::Build(m_Channels);
    if (!runtime)
    {
        std::vector<AnimChannel>().swap(m_Channels);
        m_Duration = m_PublishedDuration;
        return false;
    }
    m_PublishedDuration = m_Duration;
    std::atomic_store_explicit(&m_RuntimeData, std::move(runtime), std::memory_order_release);
    return true;
}

} // namespace GameEngine::Animation
