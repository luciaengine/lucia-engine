#include "Editor/Settings/AssetPreviewSettings.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Thumbnails/ModelThumbnailHandler.h"

#include <string>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kPrefKeyPreviewScrollZoom = "ui.previewScrollZoom";
constexpr const char* kPrefKeyPackagePreviews = "ui.packagePreviews";
bool s_PackagePreviewsEnabled = true;

void SetPackagePreviewsEnabled(bool enabled)
{
    if (s_PackagePreviewsEnabled == enabled)
        return;
    s_PackagePreviewsEnabled = enabled;
    PackagePreviewsChanged().Invoke();
}
} // namespace

bool GetPackagePreviewsEnabled()
{
    return s_PackagePreviewsEnabled;
}

Event<>& PackagePreviewsChanged()
{
    static Event<> changed;
    return changed;
}

void ApplyAssetPreviewSettingsFromPreferences()
{
    auto prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);
    bool scrollZoom = true;
    prefs.TryGetBool(kPrefKeyPreviewScrollZoom, scrollZoom);
    ModelThumbnailHandler::SetPreviewScrollZoomEnabled(scrollZoom);
    bool packagePreviews = true;
    prefs.TryGetBool(kPrefKeyPackagePreviews, packagePreviews);
    SetPackagePreviewsEnabled(packagePreviews);
}

void RegisterAssetPreviewSettingsCategory()
{
    SettingsCategoryDescriptor assetPreview;
    assetPreview.CategoryId = "assetPreview";
    assetPreview.Title = "Asset Preview";
    assetPreview.Group = SettingsCategoryGroup::UI;
    assetPreview.TreeRowClass = "ui-asset-preview-row";
    assetPreview.SearchKeywords = "asset preview scroll wheel zoom dolly model 3d";

    SettingsFieldDescriptor scrollZoom;
    scrollZoom.Label = "Scroll Wheel Zoom Preview";
    scrollZoom.Tooltip =
        "Plain mouse-wheel scroll over the Asset View 3D preview dollies in and out. "
        "When off, only the bound Asset Preview shortcuts affect the preview.";
    scrollZoom.SearchKeywords = "scroll wheel zoom preview dolly";
    scrollZoom.PrefKey = kPrefKeyPreviewScrollZoom;
    SettingsFieldDescriptor::ToggleField control;
    control.DefaultValue = true;
    control.Set = [](bool enabled)
    { ModelThumbnailHandler::SetPreviewScrollZoomEnabled(enabled); };
    scrollZoom.Control = std::move(control);
    assetPreview.Fields.push_back(std::move(scrollZoom));

    SettingsFieldDescriptor packagePreviews;
    packagePreviews.Label = "Show Package Previews";
    packagePreviews.Tooltip = "Show package icons and optional screenshots in the Packages window.";
    packagePreviews.SearchKeywords = "package preview icon screenshot artwork";
    packagePreviews.PrefKey = kPrefKeyPackagePreviews;
    SettingsFieldDescriptor::ToggleField packagePreviewsControl;
    packagePreviewsControl.DefaultValue = true;
    packagePreviewsControl.Get = GetPackagePreviewsEnabled;
    packagePreviewsControl.Set = SetPackagePreviewsEnabled;
    packagePreviews.Control = std::move(packagePreviewsControl);
    assetPreview.Fields.push_back(std::move(packagePreviews));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(assetPreview));
}

} // namespace GameEngine::Editor
