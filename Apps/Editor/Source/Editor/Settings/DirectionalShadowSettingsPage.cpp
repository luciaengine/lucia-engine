#include "Editor/Settings/DirectionalShadowSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/Render/RenderProjectSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include <string>
#include <utility>

namespace GameEngine::Editor
{
SettingsCategoryDescriptor CreateDirectionalShadowSettingsSection(
    std::function<void()> onProjectRenderSettingsChanged)
{
    SettingsCategoryDescriptor section;
    section.CategoryId = "directionalShadows";
    section.Title = "Directional Shadows";
    section.Group = SettingsCategoryGroup::ProjectSettings;
    section.Description =
        "Applies to all directional cascades. To change the cascade resolution, select the "
        "render pipeline asset and edit Resolution in its ShadowMap pass.";
    section.SearchKeywords = "shadow resolution cascade size shadowmap pipeline";

    SettingsFieldDescriptor projection;
    projection.Label = "Stable Projection";
    projection.Tooltip =
        "Keeps the cascade's size fixed as the camera moves, so shadow texels stay welded to the "
        "world instead of crawling. On, the splits come only from the shadow distance and split "
        "lambda, and the cascade is fitted to the frustum slice's bounding sphere -- both are "
        "invariant to camera movement and rotation, so the world area each shadow texel covers "
        "never changes. Off (Close Fit), the splits follow the measured depth bounds and the "
        "cascade is fitted tightly to what is actually visible: sharper shadows, but the fit "
        "resizes as you move and every texel re-quantizes at once, which reads as shimmering or "
        "dancing shadow edges. Stable costs roughly a third of the near-cascade texel density; "
        "raise the ShadowMap node resolution or split lambda in the render pipeline to win it "
        "back.";
    projection.SearchKeywords =
        "stable projection close fit shimmer shimmering crawl crawling dancing texel swim "
        "flicker cascade sphere sdsm";

    SettingsFieldDescriptor::ToggleField projectionToggle;
    projectionToggle.DefaultValue = kDefaultDirectionalShadowStableProjection;
    projectionToggle.Get = []()
    {
        SettingsStore store = OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
        std::string error;
        (void)store.Load(&error);
        return GetDirectionalShadowStableProjection(store);
    };
    projectionToggle.Set = [onChanged = std::move(onProjectRenderSettingsChanged)](bool value)
    {
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        if (workspaceRoot.empty())
        {
            Logger::Log::Warning(
                "DirectionalShadowSettings: cannot save projection mode -- no workspace root");
            return;
        }

        SettingsStore store = OpenProjectSettings(workspaceRoot);
        std::string error;
        (void)store.Load(&error);
        if (GetDirectionalShadowStableProjection(store) == value)
            return;

        SetDirectionalShadowStableProjection(store, value);
        if (!store.Save(&error))
        {
            Logger::Log::Error(
                "DirectionalShadowSettings: failed to save projection mode: {}", error);
            return;
        }
        if (onChanged)
            onChanged();
    };
    projection.Control = std::move(projectionToggle);
    section.Fields.push_back(std::move(projection));

    return section;
}

} // namespace GameEngine::Editor
