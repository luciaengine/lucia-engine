#include "Engine/Rendering/MaterialBinder.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{

// Bindings this many and above get no warn-once diagnostic, because the
// warn-once bookkeeping is a bitmask of this width. The SKIP itself is never
// gated on it — a binding past the window still drops the draw, just quietly,
// so keep this at or above the highest binding any set declares (the world
// pass's DDGI keyword currently reaches b37, Includes/ddgi_probes.glsl).
constexpr uint32_t kMaxWarnTrackedBinding = 64;

// Fibonacci-hash a cache-key hash to a power-of-two L1 slot index (256 slots).
// Mirrors PipelineVariantCache.cpp / the device PipelineCache's L1IndexFor.
inline size_t PerPassL1Index(size_t keyHash) noexcept
{
    return static_cast<size_t>(
        (static_cast<uint64_t>(keyHash) * 0x9E3779B97F4A7C15ull) >> (64 - 8));
}

using ::GameEngine::Rendering::DescriptorBinding;
using ::GameEngine::Rendering::DescriptorBindingMeta;
using ::GameEngine::Rendering::DescriptorSetDesc;
using ::GameEngine::Rendering::DescriptorSetHandle;
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
using ::GameEngine::Rendering::DescriptorSetMeta;
using ::GameEngine::Rendering::DescriptorSetUpdate;
using ::GameEngine::Rendering::DescriptorType;

using ::GameEngine::Rendering::DescriptorBindingPlan;

// Tools and tests may construct reflection by hand. Published shader variants
// already own a plan, including the previous generation served during reload.
const std::shared_ptr<const DescriptorBindingPlan>& PlanFor(
    const DescriptorSetMeta& set, std::shared_ptr<const DescriptorBindingPlan>& scratch)
{
    if (set.BindingPlan)
        return set.BindingPlan;
    scratch = BuildDescriptorBindingPlan(set);
    return scratch;
}

// Locate the reflected set with index `setIdx`. Returns nullptr if missing.
const DescriptorSetMeta* FindSetMeta(
    const ::GameEngine::Rendering::ShaderMeta& meta, uint32_t setIdx)
{
    for (const auto& s : meta.Sets)
    {
        if (s.Set == setIdx)
            return &s;
    }
    return nullptr;
}

// Linear search over a DrawBindings buffer table by name.
const DrawBindings::BufferEntry* FindDrawBuffer(const DrawBindings& draw, StringId nameId)
{
    for (const auto& e : draw.Buffers)
    {
        if (e.Name == nameId)
            return &e;
    }
    return nullptr;
}

// Linear search over a DrawBindings texture table by name.
const DrawBindings::TextureEntry* FindDrawTexture(const DrawBindings& draw, StringId nameId)
{
    for (const auto& e : draw.Textures)
    {
        if (e.Name == nameId)
            return &e;
    }
    return nullptr;
}

// Linear search over the pass's resolved buffer table by name.
const ResolvedPassResources::BufferEntry* FindPassBuffer(
    const ResolvedPassResources& pr, StringId nameId)
{
    for (const auto& e : pr.Buffers)
    {
        if (e.Name == nameId)
            return &e;
    }
    return nullptr;
}

// Linear search over the pass's resolved texture table by name.
const ResolvedPassResources::TextureEntry* FindPassTexture(
    const ResolvedPassResources& pr, StringId nameId)
{
    for (const auto& e : pr.Textures)
    {
        if (e.Name == nameId)
            return &e;
    }
    return nullptr;
}

struct ImageDefaultBinding
{
    Rendering::TextureHandle Texture;
    Rendering::SamplerHandle Sampler;
};

ImageDefaultBinding ResolveImageDefault(RenderServices& services, DescriptorBindingPlan::ImageDefault shape,
                                        std::string_view name, Rendering::SamplerHandle sampler)
{
    using Shape = DescriptorBindingPlan::ImageDefault;
    switch (shape)
    {
        case Shape::Named2D: return {services.Textures().ResolveDefaultTexture(name), sampler};
        case Shape::ColorArray: return {services.Textures().GetDefaultWhiteArrayTexture(), sampler};
        case Shape::ColorCube: return {services.Textures().GetDefaultBlackCubeTexture(), sampler};
        case Shape::Depth2D: return {services.GetAreaShadowFallbackTexture(), services.GetCascadeShadowSampler()};
        case Shape::DepthArray: return {services.GetCascadeShadowFallbackTexture(), services.GetCascadeShadowSampler()};
        case Shape::Unavailable: return {};
    }
    return {};
}

void WarnUnresolvedPipelineBinding(const DescriptorSetMeta& set, const DescriptorBindingMeta& binding,
                                  const char* kind)
{
    if (binding.Binding >= kMaxWarnTrackedBinding)
        return;
    thread_local std::unordered_map<const DescriptorSetMeta*, uint64_t> warnedBindings;
    auto& mask = warnedBindings[&set];
    const uint64_t bit = 1ull << binding.Binding;
    if ((mask & bit) != 0)
        return;
    mask |= bit;
    Logger::Log::Warning("MaterialBinder.PipelineSet: set={} binding={} ('{}') {} not resolvable",
                         set.Set, binding.Binding, binding.Name, kind);
}

} // namespace

uint64_t MaterialBinder::NextPerPassEpoch() noexcept
{
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

MaterialBinder::MaterialBinder(RenderServices& services, ::GameEngine::Rendering::IDevice& device)
    : m_Services(services)
    , m_Device(device)
    , m_PerPassEpoch(NextPerPassEpoch())
{
}

MaterialBinder::~MaterialBinder() = default;

void MaterialBinder::OnBeginFrame()
{
    // Transient descriptor sets are reset by the device's per-frame pool at
    // frame boundary; clearing the cache releases the now-stale handles so
    // the next-frame lookups allocate fresh sets. Serial (frame boundary,
    // before any parallel record window opens — A2.4-D6). Move to a fresh epoch
    // so per-thread L1 slots holding last frame's (now-stale) handles are
    // retired on their next probe.
    m_PerPassCache.clear();
    m_PerPassEpoch.store(NextPerPassEpoch(), std::memory_order_release);
}

void MaterialBinder::AssertPerPassLockHeld() const noexcept
{
#if GE_DEBUG_INSTRUMENTATION
    assert(m_PerPassInsertLockHeldDebug
           && "MaterialBinder per-pass cold insert without holding "
              "m_PerPassCacheMutex exclusively (A2.4-D6).");
#endif
}

PassBindingContext MaterialBinder::BeginPass(
    ::GameEngine::Rendering::CommandList& cmd,
    ::GameEngine::Rendering::ViewId view,
    const ::GameEngine::Rendering::RenderGraph::RGContext& passCtx,
    ::GameEngine::Rendering::MaterialKeyword passKeywords,
    ResolvedPassResources resources,
    uint32_t passInstanceIndex)
{
    PassBindingContext pass{};
    pass.Cmd               = &cmd;
    pass.View              = view;
    pass.PassRG            = &passCtx;
    pass.PassKeywords      = passKeywords;
    pass.FrameSlot         = m_Device.GetFrameIndex();
    pass.Samples           = passCtx.GetCurrentSampleCount();
    pass.PassInstanceIndex = passInstanceIndex;
    pass.PassResources     = std::move(resources);
    return pass;
}

void MaterialBinder::EndPass(PassBindingContext& pass)
{
    (void)pass;
}

void MaterialBinder::InvalidateStickyBindsForPipeline(
    PassBindingContext& pass,
    std::span<const ::GameEngine::Rendering::DescriptorSetLayoutId> pipelineSetLayouts)
{
    using namespace ::GameEngine::Rendering;

    // Vulkan's pipeline-layout-compatibility rule: when a pipeline is bound
    // whose set-N layout differs from the previously bound pipeline's,
    // descriptor-set bindings at set N and all higher sets are disturbed.
    // Sets below the first incompatibility keep their bindings. Mirror that
    // on the sticky-bind tracking so the next set walk only rebinds what
    // Vulkan actually invalidated. Without precomputed layout ids we can't
    // tell — clear everything (conservative, matches pre-optimisation
    // behavior). All entity / variant-cached call sites pass precomputed
    // ids — the fallback exists for callers that don't.
    if (pipelineSetLayouts.empty())
    {
        pass.CurrentSets.fill({});
        pass.CurrentSetLayouts.fill(DescriptorSetLayoutId{});
        return;
    }

    uint32_t firstDiff = kMaxDescriptorSets;
    for (uint32_t i = 0; i < kMaxDescriptorSets; ++i)
    {
        const auto newLayout = (i < pipelineSetLayouts.size())
            ? pipelineSetLayouts[i]
            : DescriptorSetLayoutId{};
        if (newLayout != pass.CurrentSetLayouts[i])
        {
            firstDiff = i;
            break;
        }
    }
    // Clear sticky binds for the disturbed range and update layout tracking
    // for every slot so the NEXT pipeline change has correct comparison
    // material. Sets below `firstDiff` are untouched.
    for (uint32_t i = firstDiff; i < kMaxDescriptorSets; ++i)
    {
        pass.CurrentSets[i] = {};
        pass.CurrentSetLayouts[i] = (i < pipelineSetLayouts.size())
            ? pipelineSetLayouts[i]
            : DescriptorSetLayoutId{};
    }
}

DescriptorSetHandle MaterialBinder::BuildSetForBinding(
    PassBindingContext& pass,
    const Material& material,
    uint32_t setIdx,
    const DrawBindings& draw)
{
    const auto& meta = material.GetShaderMeta();
    if (!meta)
        return {};
    const auto* setMeta = FindSetMeta(*meta, setIdx);
    if (!setMeta || setMeta->Bindings.empty())
        return {};
    return BuildSetForBindingFromMeta(pass, material, *setMeta, draw);
}

DescriptorSetHandle MaterialBinder::BuildSetForBindingFromMeta(
    const PassBindingContext& pass,
    const Material& material,
    const ::GameEngine::Rendering::DescriptorSetMeta& setMeta,
    const DrawBindings& draw,
    bool* outUnresolvable)
{
    if (setMeta.Bindings.empty())
        return {};

    std::shared_ptr<const DescriptorBindingPlan> scratchPlan;
    const auto& plan = PlanFor(setMeta, scratchPlan);
    if (plan->SetSource == DescriptorBindingPlan::Source::BindlessTextures && m_Services.Textures().IsBindlessEnabled())
        return m_Services.Textures().BindlessTextureSet();
    if (plan->SetSource == DescriptorBindingPlan::Source::MaterialTextures)
    {
        if (auto set = m_Services.Textures().MaterialTextureSet(material); set.IsValid())
            return set;
    }

    thread_local std::vector<ResolvedBinding> writes;
    const auto resolution = ResolveBindings(pass.PassResources, material, setMeta.Set, *plan, draw, writes);
    if (outUnresolvable && resolution.Unresolvable)
        *outUnresolvable = true;
    if (resolution.PerDraw || resolution.Unresolvable)
        return BuildSetInternal(setMeta, *plan, writes);

    using Rendering::HashUtils::HashCombine;
    uint64_t digest = setMeta.BuiltLayoutHash ? setMeta.BuiltLayoutHash : ComputeDescriptorSetLayoutHash(setMeta);
    for (const auto& write : writes)
    {
        digest = HashCombine(digest, write.Buffer.id);
        digest = HashCombine(digest, write.Offset);
        digest = HashCombine(digest, write.Range);
        digest = HashCombine(digest, write.Texture.id);
        digest = HashCombine(digest, write.Sampler.id);
        digest = HashCombine(digest, write.ArrayIndex);
    }
    const PerPassCacheKey key{static_cast<uint64_t>(pass.View), static_cast<uint64_t>(pass.PassKeywords),
                              digest, setMeta.Set, pass.PassInstanceIndex};
    const uint64_t epoch = m_PerPassEpoch.load(std::memory_order_acquire);
    thread_local std::array<PerPassL1Slot, kPerPassL1Slots> l1{};
    auto& slot = l1[PerPassL1Index(PerPassCacheKeyHash{}(key))];
    if (slot.Epoch == epoch && slot.Value && slot.Key == key && slot.Value->Matches(*plan, writes))
        return slot.Value->Handle;

    // unordered_multimap keeps node addresses stable across insertion and
    // rehash. OnBeginFrame retires all nodes serially, before workers run.
    {
        std::shared_lock lock(m_PerPassCacheMutex);
        if (const auto* found = FindCachedSet(key, *plan, writes))
        {
            slot = {key, found, epoch};
            return found->Handle;
        }
    }

    auto handle = BuildSetInternal(setMeta, *plan, writes);
    if (!handle.IsValid())
        return {};
    std::unique_lock lock(m_PerPassCacheMutex);
#if GE_DEBUG_INSTRUMENTATION
    m_PerPassInsertLockHeldDebug = true;
#endif
    AssertPerPassLockHeld();
    // A competing worker may already have published the same contents while
    // the device allocated our transient descriptor outside the lock.
    const auto* found = FindCachedSet(key, *plan, writes);
    if (!found)
        found = &m_PerPassCache.emplace(key, CachedSet{plan, writes, handle})->second;
    slot = {key, found, epoch};
#if GE_DEBUG_INSTRUMENTATION
    m_PerPassInsertLockHeldDebug = false;
#endif
    return found->Handle;
}

bool MaterialBinder::CachedSet::Matches(const DescriptorBindingPlan& plan,
                                        const std::vector<ResolvedBinding>& writes) const
{
    return (Plan.get() == &plan || *Plan == plan) && Writes == writes;
}

const MaterialBinder::CachedSet* MaterialBinder::FindCachedSet(
    const PerPassCacheKey& key, const DescriptorBindingPlan& plan,
    const std::vector<ResolvedBinding>& writes) const
{
    const auto [first, last] = m_PerPassCache.equal_range(key);
    for (auto entry = first; entry != last; ++entry)
    {
        if (entry->second.Matches(plan, writes))
            return &entry->second;
    }
    return nullptr;
}

Rendering::SamplerHandle MaterialBinder::ResolveMaterialSampler(
    const Material& material, Rendering::SamplerHandle& cachedSampler)
{
    if (!cachedSampler.IsValid())
        cachedSampler = m_Services.Textures().GetSampler(material.GetSamplerPreset());
    return cachedSampler;
}

bool MaterialBinder::ShouldWarnBinding(const Material& material, uint32_t setIndex, uint32_t binding)
{
    if (binding >= kMaxWarnTrackedBinding)
        return false;
    thread_local std::unordered_map<WarnKey, uint64_t, WarnKeyHash> warnedBindings;
    auto& mask = warnedBindings[WarnKey{&material, setIndex}];
    const uint64_t bit = 1ull << binding;
    const bool first = (mask & bit) == 0;
    mask |= bit;
    return first;
}

MaterialBinder::Resolution MaterialBinder::ResolveBindings(
    const ResolvedPassResources& pass, const Material& material, uint32_t setIndex,
    const DescriptorBindingPlan& plan, const DrawBindings& draw, std::vector<ResolvedBinding>& writes)
{
    using namespace ::GameEngine::Rendering;
    Resolution result{};
    writes.clear();
    writes.reserve(plan.Slots.size());
    SamplerHandle materialSampler{};
    for (const auto& slot : plan.Slots)
    {
        auto& write = writes.emplace_back();
        switch (slot.Type)
        {
            case DescriptorType::UniformBuffer:
            case DescriptorType::StorageBuffer:
                if (const auto* entry = FindDrawBuffer(draw, slot.NameId))
                {
                    result.PerDraw = true;
                    write.Buffer = entry->Buffer;
                    write.Offset = entry->Offset;
                    write.Range = entry->Range;
                }
                else if (const auto* entry = FindPassBuffer(pass, slot.NameId))
                {
                    write.Buffer = entry->Buffer;
                    write.Offset = entry->Offset;
                    write.Range = entry->Range;
                }
                if (!write.Buffer.IsValid())
                {
                    result.Unresolvable = true;
                    if (ShouldWarnBinding(material, setIndex, slot.Binding))
                        Logger::Log::Error("MaterialBinder: set={} binding={} ('{}') has no valid buffer — SKIPPING the draw",
                                           setIndex, slot.Binding, slot.Name);
                }
                break;

            case DescriptorType::Texture:
            case DescriptorType::CombinedImageSampler:
            {
                if (const auto* entry = FindDrawTexture(draw, slot.NameId); entry && entry->Texture.IsValid())
                {
                    result.PerDraw = true;
                    write.Texture = entry->Texture;
                    write.Sampler = entry->Sampler.IsValid()
                        ? entry->Sampler : ResolveMaterialSampler(material, materialSampler);
                    write.ArrayIndex = entry->ArrayIndex;
                }
                else if (const auto* entry = FindPassTexture(pass, slot.NameId); entry && entry->Texture.IsValid())
                {
                    write.Texture = entry->Texture;
                    if (slot.Type == DescriptorType::CombinedImageSampler)
                        write.Sampler = entry->Sampler.IsValid()
                            ? entry->Sampler : ResolveMaterialSampler(material, materialSampler);
                }
                else
                {
                    const auto fallback = ResolveImageDefault(
                        m_Services, slot.Fallback, slot.Name, ResolveMaterialSampler(material, materialSampler));
                    write.Texture = fallback.Texture;
                    if (slot.Type == DescriptorType::CombinedImageSampler)
                        write.Sampler = fallback.Sampler;
                    if (!write.Texture.IsValid())
                        result.Unresolvable = true;
                    if (ShouldWarnBinding(material, setIndex, slot.Binding))
                    {
                        if (write.Texture.IsValid())
                            Logger::Log::Warning("MaterialBinder: set={} binding={} ('{}') texture not resolvable — bound the slot default",
                                                 setIndex, slot.Binding, slot.Name);
                        else
                            Logger::Log::Error("MaterialBinder: set={} binding={} ('{}') texture not resolvable and its shape has no slot default — SKIPPING the draw",
                                               setIndex, slot.Binding, slot.Name);
                    }
                }
                break;
            }
            case DescriptorType::Sampler:
                if (const auto* entry = FindDrawTexture(draw, slot.NameId); entry && entry->Sampler.IsValid())
                {
                    result.PerDraw = true;
                    write.Sampler = entry->Sampler;
                }
                else if (const auto* entry = FindPassTexture(pass, slot.NameId); entry && entry->Sampler.IsValid())
                    write.Sampler = entry->Sampler;
                else
                {
                    write.Sampler = ResolveMaterialSampler(material, materialSampler);
                    if (ShouldWarnBinding(material, setIndex, slot.Binding))
                        Logger::Log::Warning("MaterialBinder: set={} binding={} ('{}') sampler not resolvable — bound the material sampler",
                                             setIndex, slot.Binding, slot.Name);
                }
                break;

            case DescriptorType::AccelerationStructure:
                result.Unresolvable = true;
                if (ShouldWarnBinding(material, setIndex, slot.Binding))
                    Logger::Log::Error("MaterialBinder: set={} binding={} ('{}') has no acceleration structure provider — SKIPPING the draw",
                                       setIndex, slot.Binding, slot.Name);
                break;
            default:
                break;
        }
    }
    return result;
}

DescriptorSetHandle MaterialBinder::BuildSetInternal(
    const DescriptorSetMeta& setMeta, const DescriptorBindingPlan& plan, std::span<const ResolvedBinding> writes)
{
    using namespace ::GameEngine::Rendering;
    auto layout = MaterialBuilder::BuildSetLayout(setMeta);
    if (std::any_of(layout.bindings.begin(), layout.bindings.end(), [](const DescriptorBinding& binding) {
            return (binding.flags & kDescriptorBindingUpdateAfterBind) != 0;
        }))
        return {};
    DescriptorSetDesc desc{};
    desc.layout = std::move(layout);
    desc.transient = true;
    desc.debugName = "MaterialBinder.Set";
    const auto handle = m_Device.CreateDescriptorSet(desc);
    if (!handle.IsValid())
        return {};

    thread_local std::vector<DescriptorSetUpdate> updates;
    updates.clear();
    for (size_t bindingIndex = 0; bindingIndex < writes.size(); ++bindingIndex)
    {
        const auto& slot = plan.Slots[bindingIndex];
        const auto& write = writes[bindingIndex];
        if (write.Buffer.IsValid())
        {
            auto& update = updates.emplace_back();
            update.binding = slot.Binding;
            update.type = slot.Type;
            update.buffers = {write.Buffer};
            if (write.Offset || write.Range)
            {
                update.bufferOffsets = {static_cast<size_t>(write.Offset)};
                update.bufferRanges = {static_cast<size_t>(write.Range)};
            }
        }
        else if (write.Texture.IsValid())
        {
            if (slot.Type == DescriptorType::CombinedImageSampler && write.Sampler.IsValid())
                m_Device.UpdateCombinedImageSamplerBinding(handle, slot.Binding, write.Texture, write.Sampler, write.ArrayIndex);
            else
            {
                auto& update = updates.emplace_back();
                update.binding = slot.Binding;
                update.arrayElement = write.ArrayIndex;
                update.type = slot.Type;
                update.textures = {write.Texture};
                if (write.Sampler.IsValid())
                    update.samplers = {write.Sampler};
            }
        }
        else if (write.Sampler.IsValid())
            m_Device.UpdateSamplerBinding(handle, slot.Binding, write.Sampler);
    }
    if (!updates.empty())
        m_Device.UpdateDescriptorSetBatch(handle, updates);
    return handle;
}

namespace
{
PreparedDrawBindings MakePreparedDraw(Rendering::PipelineHandle pipeline,
                                      std::span<const Rendering::DescriptorSetLayoutId> layouts)
{
    PreparedDrawBindings result{};
    result.Pipeline = pipeline;
    result.LayoutCount = static_cast<uint32_t>(std::min(layouts.size(), result.Layouts.size()));
    std::copy_n(layouts.begin(), result.LayoutCount, result.Layouts.begin());
    return result;
}

bool ShouldBindSet(const Rendering::DescriptorSetMeta& set, uint32_t pipelineSetCount)
{
    return !set.Bindings.empty() && set.Set < kMaxDescriptorSets
        && (pipelineSetCount == 0 || set.Set < pipelineSetCount);
}
} // namespace

PreparedDrawBindings MaterialBinder::PrepareMaterialForDraw(
    const PassBindingContext& pass, const Material& material, Rendering::PipelineHandle pipeline,
    const Rendering::ShaderMeta* servedMeta, const DrawBindings& draw, uint32_t pipelineSetCount,
    std::span<const Rendering::DescriptorSetLayoutId> pipelineSetLayouts)
{
    if (!pipeline.IsValid())
        return {};
    auto prepared = MakePreparedDraw(pipeline, pipelineSetLayouts);
    const auto* meta = servedMeta ? servedMeta : material.GetShaderMeta().get();
    if (meta)
    {
        for (const auto& set : meta->Sets)
        {
            if (!ShouldBindSet(set, pipelineSetCount))
                continue;
            bool unresolvable = false;
            prepared.Sets[set.Set] = BuildSetForBindingFromMeta(pass, material, set, draw, &unresolvable);
            if (unresolvable)
                return {};
        }
    }
    return prepared;
}

PreparedDrawBindings MaterialBinder::PreparePipelineForDraw(
    const PassBindingContext& pass, Rendering::PipelineHandle pipeline, const Rendering::ShaderMeta& meta,
    const DrawBindings& draw, uint32_t pipelineSetCount,
    std::span<const Rendering::DescriptorSetLayoutId> pipelineSetLayouts)
{
    using namespace ::GameEngine::Rendering;
    if (!pipeline.IsValid())
        return {};
    auto prepared = MakePreparedDraw(pipeline, pipelineSetLayouts);
    for (const auto& setMeta : meta.Sets)
    {
        if (!ShouldBindSet(setMeta, pipelineSetCount))
            continue;
        DescriptorSetHandle setHandle{};
        std::shared_ptr<const DescriptorBindingPlan> scratchPlan;
        const auto& plan = PlanFor(setMeta, scratchPlan);
        if (plan->SetSource == DescriptorBindingPlan::Source::BindlessTextures)
        {
            if (m_Services.Textures().IsBindlessEnabled())
                setHandle = m_Services.Textures().BindlessTextureSet();
        }
        else if (plan->SetSource == DescriptorBindingPlan::Source::MaterialTextures)
        {
            setHandle = m_Services.Textures().DefaultMaterialTextureSet();
        }
        if (!setHandle.IsValid())
            setHandle = BuildPipelineSet(pass, setMeta, *plan, draw);
        prepared.Sets[setMeta.Set] = setHandle;
    }
    return prepared;
}

Rendering::PipelineHandle MaterialBinder::BindPreparedForDraw(
    PassBindingContext& pass, const PreparedDrawBindings& prepared, std::span<const std::byte> pushConstants)
{
    assert(pass.Cmd && "BindPreparedForDraw requires a command list");
    if (!pass.Cmd || !prepared.Pipeline.IsValid())
        return {};
    if (prepared.Pipeline != pass.CurrentPipeline)
    {
        pass.Cmd->SetPipeline(prepared.Pipeline);
        pass.CurrentPipeline = prepared.Pipeline;
        ++pass.PipelineBinds;
        InvalidateStickyBindsForPipeline(pass, std::span(prepared.Layouts).first(prepared.LayoutCount));
    }
    for (uint32_t setIndex = 0; setIndex < kMaxDescriptorSets; ++setIndex)
    {
        const auto handle = prepared.Sets[setIndex];
        if (handle.IsValid() && handle != pass.CurrentSets[setIndex])
        {
            pass.Cmd->BindDescriptorSet(setIndex, handle, prepared.Pipeline);
            pass.CurrentSets[setIndex] = handle;
            ++pass.DescriptorBinds;
        }
    }
    if (!pushConstants.empty())
        pass.Cmd->SetConstants(0, 0, pushConstants.size(), pushConstants.data());
    return prepared.Pipeline;
}

::GameEngine::Rendering::DescriptorSetHandle MaterialBinder::BuildPipelineSet(
    const PassBindingContext& pass,
    const ::GameEngine::Rendering::DescriptorSetMeta& setMeta,
    const DescriptorBindingPlan& plan, const DrawBindings& draw)
{
    using namespace ::GameEngine::Rendering;

    auto layout = ::GameEngine::Rendering::MaterialBuilder::BuildSetLayout(setMeta);

    // UAB layouts can only come from the persistent pool. The bindless
    // short-circuit upstream catches the engine's canonical case; this is a
    // defensive guard mirroring BuildSetInternal.
    const bool needsUpdateAfterBind = std::any_of(
        layout.bindings.begin(), layout.bindings.end(),
        [](const DescriptorBinding& binding) {
            return (binding.flags & kDescriptorBindingUpdateAfterBind) != 0;
        });
    if (needsUpdateAfterBind)
        return {};

    DescriptorSetDesc descriptor{};
    descriptor.layout    = std::move(layout);
    descriptor.transient = true;
    descriptor.debugName = "MaterialBinder.PipelineSet";
    auto handle = m_Device.CreateDescriptorSet(descriptor);
    if (!handle.IsValid())
        return {};

    // Each preparation thread reuses its own descriptor-update storage.
    thread_local std::vector<DescriptorSetUpdate> updates;
    updates.clear();
    updates.reserve(setMeta.Bindings.size());

    for (size_t bindingIndex = 0; bindingIndex < plan.Slots.size(); ++bindingIndex)
    {
        const auto& binding = setMeta.Bindings[bindingIndex];
        const auto& slot = plan.Slots[bindingIndex];
        const StringId nameId = slot.NameId;
        const DescriptorType type = slot.Type;

        switch (type)
        {
            case DescriptorType::UniformBuffer:
            case DescriptorType::StorageBuffer:
            {
                if (const auto* entry = FindDrawBuffer(draw, nameId);
                    entry && entry->Buffer.IsValid())
                {
                    DescriptorSetUpdate update{};
                    update.binding = binding.Binding;
                    update.type    = type;
                    update.buffers = {entry->Buffer};
                    if (entry->Offset != 0 || entry->Range != 0)
                    {
                        update.bufferOffsets = {static_cast<size_t>(entry->Offset)};
                        update.bufferRanges  = {static_cast<size_t>(entry->Range)};
                    }
                    updates.push_back(std::move(update));
                    break;
                }
                if (const auto* entry = FindPassBuffer(pass.PassResources, nameId);
                    entry && entry->Buffer.IsValid())
                {
                    DescriptorSetUpdate update{};
                    update.binding = binding.Binding;
                    update.type    = type;
                    update.buffers = {entry->Buffer};
                    if (entry->Offset != 0 || entry->Range != 0)
                    {
                        update.bufferOffsets = {static_cast<size_t>(entry->Offset)};
                        update.bufferRanges  = {static_cast<size_t>(entry->Range)};
                    }
                    updates.push_back(std::move(update));
                    break;
                }
                WarnUnresolvedPipelineBinding(setMeta, binding, "buffer");
                break;
            }
            case DescriptorType::CombinedImageSampler:
            case DescriptorType::Texture:
            {
                if (const auto* entry = FindDrawTexture(draw, nameId);
                    entry && entry->Texture.IsValid() && entry->Sampler.IsValid())
                {
                    m_Device.UpdateCombinedImageSamplerBinding(
                        handle, binding.Binding, entry->Texture, entry->Sampler, entry->ArrayIndex);
                    break;
                }
                if (const auto* entry = FindPassTexture(pass.PassResources, nameId);
                    entry && entry->Texture.IsValid() && entry->Sampler.IsValid())
                {
                    m_Device.UpdateCombinedImageSamplerBinding(
                        handle, binding.Binding, entry->Texture, entry->Sampler);
                    break;
                }
                WarnUnresolvedPipelineBinding(setMeta, binding, "texture");
                break;
            }
            case DescriptorType::AccelerationStructure:
                // No pipeline-set provider exists for acceleration structures.
                WarnUnresolvedPipelineBinding(setMeta, binding, "acceleration structure");
                break;
            default:
                break;
        }
    }

    if (!updates.empty())
        m_Device.UpdateDescriptorSetBatch(handle, updates);

    return handle;
}

} // namespace GameEngine::Engine::Renderer
