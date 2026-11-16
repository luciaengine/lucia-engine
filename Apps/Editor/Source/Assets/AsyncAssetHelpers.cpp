#include "Editor/Assets/AsyncAssetHelpers.h"

#include "Assets/AssetManager.h"
#include "ECS/Entity.h"
#include "UI/UIElement.h"

#include <chrono>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

struct PendingLoad
{
    AssetFuture Future;
    bool HasTarget = false;
    UIElement::WeakRef<UIElement> Target;
    const ECS::World* World = nullptr;
    uint64 WorldGeneration = 0;
    std::function<void()> OnReady;
    std::string Label;
    // Set by RunWhenReady in place of a load: polled each frame until it returns true.
    std::function<bool()> IsReady;
};

// Main thread only: filled by RunWhenAssetLoaded, drained by PollPendingAssetLoads.
std::vector<PendingLoad>& PendingLoads()
{
    static std::vector<PendingLoad> s_PendingLoads;
    return s_PendingLoads;
}

// True when the continuation's target or world is gone: it is dropped without running.
bool IsAbandoned(const PendingLoad& load)
{
    if (load.HasTarget && !load.Target.Get())
        return true;
    // The world was cleared for another scene: what the drop aimed at is gone.
    return load.World && load.World->GetLifecycleResetGeneration() != load.WorldGeneration;
}

bool HasFinished(PendingLoad& load)
{
    if (IsAbandoned(load))
        return true;
    if (load.IsReady)
        return load.IsReady();
    return !load.Future.Valid() ||
           load.Future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

} // namespace

void RunWhenAssetLoaded(AssetManager& assets,
                        const GUID& guid,
                        AssetLoadPriority priority,
                        UIElement* postTarget,
                        const ECS::World* world,
                        std::function<void()> onReady,
                        std::string pendingLabel)
{
    if (!onReady || guid.IsNull())
        return;

    if (assets.IsAssetLoaded(guid))
    {
        onReady();
        return;
    }

    // The continuation runs from the main thread's poll, never from the thread
    // that finishes the load: it touches the world and the UI.
    PendingLoads().push_back(PendingLoad{assets.LoadAssetAsync(guid, priority), postTarget != nullptr,
                                         UIElement::MakeWeakRef(postTarget), world,
                                         world ? world->GetLifecycleResetGeneration() : 0, std::move(onReady),
                                         std::move(pendingLabel)});
}

void RunWhenReady(std::function<bool()> isReady,
                  UIElement* postTarget,
                  const ECS::World* world,
                  std::function<void()> onReady,
                  std::string pendingLabel)
{
    if (!onReady || !isReady)
        return;

    if (isReady())
    {
        onReady();
        return;
    }

    PendingLoads().push_back(PendingLoad{AssetFuture({}, nullptr, GUID{}), postTarget != nullptr, UIElement::MakeWeakRef(postTarget),
                                         world, world ? world->GetLifecycleResetGeneration() : 0,
                                         std::move(onReady), std::move(pendingLabel), std::move(isReady)});
}

void PollPendingAssetLoads()
{
    std::vector<PendingLoad>& pending = PendingLoads();
    std::vector<PendingLoad> finished;
    for (auto it = pending.begin(); it != pending.end();)
    {
        if (HasFinished(*it))
        {
            finished.push_back(std::move(*it));
            it = pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Taken out of the list first: a continuation may start loads of its own.
    for (PendingLoad& load : finished)
    {
        if (!IsAbandoned(load))
            load.OnReady();
    }
}

std::vector<std::string> PendingAssetLoadLabels()
{
    std::vector<std::string> labels;
    for (const PendingLoad& load : PendingLoads())
    {
        if (!load.Label.empty())
            labels.push_back(load.Label);
    }
    return labels;
}

void ClearPendingAssetLoads()
{
    PendingLoads().clear();
}

} // namespace GameEngine::Editor
