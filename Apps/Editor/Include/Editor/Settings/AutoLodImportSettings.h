#pragma once

#include <filesystem>

namespace GameEngine::Editor
{

// The project's automatic LOD import policy (project setting import.autoGenerateLODs): whether a model
// import generates simplified mesh LODs. Project-scoped, so the whole team imports alike. The engine
// import path reads the applied value (LODImportSettings::AutoGenerateOnImport). Main thread.
class AutoLodImportSettings
{
  public:
    // A project with no stored value generates LODs for its static meshes.
    static constexpr bool kDefault = true;

    // The stored policy of the project at `workspaceRoot`, or kDefault when it stores none.
    static bool Load(const std::filesystem::path& workspaceRoot);

    // Stores `enabled` in the project at `workspaceRoot` and applies it to the engine import path. An
    // unchanged value writes nothing.
    static void SaveAndApply(const std::filesystem::path& workspaceRoot, bool enabled);

    // Applies the stored policy without rewriting the settings file. Called when a project opens.
    static void ApplySaved(const std::filesystem::path& workspaceRoot);
};

} // namespace GameEngine::Editor
