#include "Editor/Settings/AutoLodImportSettings.h"

#include "Assets/MeshLODGenerator.h"
#include "Editor/Settings/SettingsStore.h"

#include <string>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kProjectKey = "import.autoGenerateLODs";

void ApplyToImportPath(bool enabled)
{
    LODImportSettings lod = GetLODImportSettings();
    lod.AutoGenerateOnImport = enabled;
    SetLODImportSettings(lod);
}
} // namespace

bool AutoLodImportSettings::Load(const std::filesystem::path& workspaceRoot)
{
    bool enabled = kDefault;
    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string error;
    (void)store.Load(&error);
    store.TryGetBool(kProjectKey, enabled);
    return enabled;
}

void AutoLodImportSettings::SaveAndApply(const std::filesystem::path& workspaceRoot, bool enabled)
{
    ApplyToImportPath(enabled);
    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string error;
    (void)store.Load(&error);
    bool stored = kDefault;
    if (store.TryGetBool(kProjectKey, stored) && stored == enabled)
        return;
    store.SetBool(kProjectKey, enabled);
    (void)store.Save(&error);
}

void AutoLodImportSettings::ApplySaved(const std::filesystem::path& workspaceRoot)
{
    ApplyToImportPath(Load(workspaceRoot));
}

} // namespace GameEngine::Editor
