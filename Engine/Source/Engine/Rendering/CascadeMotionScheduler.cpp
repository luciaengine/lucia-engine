#include "Engine/Rendering/CascadeMotionScheduler.h"

#include "Engine/Rendering/CascadeShadowCache.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <span>

namespace GameEngine
{
namespace Engine::Renderer
{

CascadeUpdateClass ClassifyCascadeDirtyCause(CascadeCacheDirtyCause cause)
{
    switch (cause)
    {
    case CascadeCacheDirtyCause::Cached:
        return CascadeUpdateClass::Cached;
    case CascadeCacheDirtyCause::CameraChanged:
    case CascadeCacheDirtyCause::CascadeFitChanged:
    case CascadeCacheDirtyCause::CasterDeformed:
        return CascadeUpdateClass::Motion;
    default:
        // FirstRender, ContributorPresent, ExecUnderDraw, ContributorChanged,
        // PhysicalChanged, CasterContentChanged, ConfigChanged, CullNotSettled:
        // the retained layer is (or may be) WRONG, not merely stale — render now.
        return CascadeUpdateClass::Correctness;
    }
}

CascadeMotionBudget FallbackCascadeMotionBudget(CascadeMotionBudget configured)
{
    constexpr uint32_t kFallbackCap = 1;
    constexpr uint32_t kFallbackMaxAge = 3;
    if (configured.Cap == 0)
        return configured;
    return {std::min(configured.Cap, kFallbackCap), std::max(configured.MaxAge, kFallbackMaxAge)};
}

CascadeUpdateClass ClassifyCascadePair(CascadeCacheDirtyCause depthCause,
                                       const CascadeCacheDirtyCause* tintCause,
                                       bool contentFitsLockstep)
{
    CascadeUpdateClass cls = ClassifyCascadeDirtyCause(depthCause);
    if (!tintCause)
        return cls;
    const CascadeUpdateClass tintCls = ClassifyCascadeDirtyCause(*tintCause);
    if (tintCls > cls)
        cls = tintCls;
    // Deferral keeps BOTH retained layers under the depth-content fit; a
    // Motion pair whose families hold content from DIFFERENT fits must render
    // instead (see the header contract).
    if (cls == CascadeUpdateClass::Motion && !contentFitsLockstep)
        cls = CascadeUpdateClass::Correctness;
    return cls;
}

namespace
{
bool IsCameraMotionCause(CascadeCacheDirtyCause cause)
{
    return cause == CascadeCacheDirtyCause::CameraChanged ||
           cause == CascadeCacheDirtyCause::CascadeFitChanged;
}
} // namespace

bool IsCameraMotionPair(CascadeUpdateClass pairClass, CascadeCacheDirtyCause depthCause,
                        const CascadeCacheDirtyCause* tintCause)
{
    if (pairClass != CascadeUpdateClass::Motion)
        return false;
    return IsCameraMotionCause(depthCause) || (tintCause && IsCameraMotionCause(*tintCause));
}

namespace
{
// The slots outside the budget under CasterDeformation, as a defer mask. A slot past the bound
// renders. The others queue in deadline order (most stale first, then nearest) and render one
// per following frame; a slot renders this frame only when the queue would otherwise break the
// bound: the slot at queue position q defers q more frames, so it needs q <= maxAge - staleness.
// After a lockstep start the slots fall into a staggered cycle with no frame that renders them all.
uint32_t StaggerOutsideBudget(std::span<uint32_t> slots,
                              const std::array<uint64_t, CascadeMotionScheduler::kMaxCascades>& staleness,
                              uint32_t maxAge)
{
    std::sort(slots.begin(), slots.end(), [&](uint32_t a, uint32_t b)
              { return staleness[a] != staleness[b] ? staleness[a] > staleness[b] : a < b; });
    size_t head = 0;
    while (head < slots.size() && staleness[slots[head]] > maxAge)
        ++head;
    for (;;)
    {
        bool feasible = true;
        for (size_t q = 0; head + q < slots.size(); ++q)
            feasible = feasible && staleness[slots[head + q]] + q <= maxAge;
        if (feasible || head == slots.size())
            break;
        ++head;
    }
    uint32_t deferMask = 0;
    for (size_t i = head; i < slots.size(); ++i)
        deferMask |= 1u << slots[i];
    return deferMask;
}
} // namespace

CascadeMotionScheduler::FramePlan CascadeMotionScheduler::PlanFrame(
    uint32_t viewId, uint32_t numCascades,
    const std::array<CascadeUpdateClass, kMaxCascades>& classes,
    const std::array<uint64_t, kMaxCascades>& staleness, uint32_t motionCap, uint32_t maxAge,
    CascadeMotionSource source, uint32_t protectedFitMask)
{
    assert(numCascades <= kMaxCascades);
    numCascades = std::min(numCascades, kMaxCascades);

    FramePlan plan{};
    if (motionCap == 0)
        return plan;

    // Cached and Correctness slots resolve outside the budget: Cached content
    // equals the current fit (byte-equality is the cache's skip condition) and
    // a correctness render replaces the content this frame.
    uint32_t motionSlots[kMaxCascades];
    uint32_t motionCount = 0;
    for (uint32_t c = 0; c < numCascades; ++c)
    {
        if (classes[c] == CascadeUpdateClass::Motion)
            motionSlots[motionCount++] = c;
    }
    if (motionCount == 0)
        return plan;

    Stats& stats = m_Views[viewId];

    // A slot past the staleness bound always renders, most stale first; the rest
    // take the budget nearest-first or defer. A forced slot takes budget under
    // Camera. Under CasterDeformation the slots outside the budget render in
    // deadline order, one per frame where the bound allows (StaggerOutsideBudget).
    const auto forced = [&](uint32_t c) { return staleness[c] > maxAge; };
    std::sort(motionSlots, motionSlots + std::min(motionCount, kMaxCascades),
              [&](uint32_t a, uint32_t b)
              {
                  const bool fa = forced(a);
                  const bool fb = forced(b);
                  if (fa != fb)
                      return fa; // forced slots first
                  if (fa && staleness[a] != staleness[b])
                      return staleness[a] > staleness[b]; // most stale first
                  return a < b;                           // nearest-first otherwise
              });
    const bool forcedTakesBudget = source == CascadeMotionSource::Camera;
    uint32_t budget = motionCap;
    uint32_t outside[kMaxCascades];
    uint32_t outsideCount = 0;
    for (uint32_t i = 0; i < motionCount; ++i)
    {
        const uint32_t c = motionSlots[i];
        const bool isForced = forced(c);
        if (isForced)
            ++stats.ForcedByAge;
        if (budget > 0 && (!isForced || forcedTakesBudget))
        {
            --budget;
            continue;
        }
        if (!forcedTakesBudget)
        {
            outside[outsideCount++] = c;
            continue;
        }
        if (!isForced)
            plan.DeferMask |= 1u << c;
    }
    plan.DeferMask |= StaggerOutsideBudget({outside, outsideCount}, staleness, maxAge);
    if (motionCap == 1u && source == CascadeMotionSource::Camera)
        plan.DeferMask &= ~protectedFitMask;

    const auto deferred = static_cast<uint32_t>(std::popcount(plan.DeferMask));
    const uint32_t renders = motionCount - deferred;
    stats.Deferred += deferred;
    if (renders > motionCap)
        stats.ForcedBeyondCap += renders - motionCap;
    return plan;
}

void CascadeMotionScheduler::Reset()
{
    m_Views.clear();
}

const CascadeMotionScheduler::Stats& CascadeMotionScheduler::GetStats(uint32_t viewId) const
{
    static const Stats kZero{};
    const auto it = m_Views.find(viewId);
    return it != m_Views.end() ? it->second : kZero;
}

} // namespace Engine::Renderer
} // namespace GameEngine
