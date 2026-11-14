#include "Editor/Settings/AssetImportSettingsPage.h"

#include "Assets/MeshLODGenerator.h"
#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/FbxImportSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Settings/SvgRasterSettings.h"
#include "Editor/Settings/SvgRasterSizeField.h"

#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
// Project-scoped so the whole team imports alike; read by the import path.
constexpr const char* kProjectKeyAutoGenerateLods = "import.autoGenerateLODs";
// Static meshes get LODs unless the project says otherwise.
constexpr bool kDefaultAutoGenerateLods = true;

using FbxFlag = bool FbxImportSettings::*;

const std::filesystem::path& WorkspaceRoot()
{
    return EngineCore::GetInstance().GetWorkspaceRoot();
}

SettingsFieldDescriptor MakeFbxToggleRow(const char* label, const char* tooltip, const char* searchKeywords,
                                         FbxFlag flag)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.Tooltip = tooltip;
    field.SearchKeywords = searchKeywords;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = FbxImportSettings{}.*flag;
    toggle.Get = [flag]() { return FbxImportSettings::Load(WorkspaceRoot()).*flag; };
    // Fired with the stored value when the page opens, which must not write
    // the project file.
    toggle.Set = [flag](bool value)
    {
        FbxImportSettings settings = FbxImportSettings::Load(WorkspaceRoot());
        if (settings.*flag == value)
            return;
        settings.*flag = value;
        settings.Save(WorkspaceRoot());
    };
    field.Control = std::move(toggle);
    return field;
}

bool LoadAutoGenerateLods()
{
    bool autoGenerate = kDefaultAutoGenerateLods;
    SettingsStore store = OpenProjectSettings(WorkspaceRoot());
    std::string err;
    (void)store.Load(&err);
    store.TryGetBool(kProjectKeyAutoGenerateLods, autoGenerate);
    return autoGenerate;
}

// Persists the project policy and pushes it to the engine import path, which
// has no editor dependency. With it off, LODs are still available from the
// model inspector and the API.
void SaveAutoGenerateLods(bool value)
{
    if (LoadAutoGenerateLods() == value)
        return;
    SettingsStore store = OpenProjectSettings(WorkspaceRoot());
    std::string err;
    (void)store.Load(&err);
    store.SetBool(kProjectKeyAutoGenerateLods, value);
    (void)store.Save(&err);
    LODImportSettings lod = GetLODImportSettings();
    lod.AutoGenerateOnImport = value;
    SetLODImportSettings(lod);
}
} // namespace

void RegisterAssetImportSettingsCategory()
{
    SettingsCategoryDescriptor assetImport;
    assetImport.CategoryId = "assetImport";
    assetImport.Title = "Asset Import";
    assetImport.Group = SettingsCategoryGroup::ProjectSettings;
    assetImport.TreeRowClass = "asset-import-row";
    assetImport.SearchKeywords = "asset import fbx svg lod model texture";

    SettingsFieldDescriptor svgSize = MakeSvgRasterSizeField(SvgRasterSettingsScope::TextureImport);
    svgSize.SectionHeader = "SVG Texture Import";
    assetImport.Fields.push_back(std::move(svgSize));

    SettingsFieldDescriptor sceneExtras = MakeFbxToggleRow(
        "Scene Extras", "Import auxiliary FBX scene data such as cameras, lights, and helper nodes.",
        "fbx import extras cameras lights lamps helper nodes empties", &FbxImportSettings::ImportSceneExtras);
    sceneExtras.SectionHeader = "FBX Import";
    assetImport.Fields.push_back(std::move(sceneExtras));
    assetImport.Fields.push_back(MakeFbxToggleRow("Cameras", "Create camera entities from imported FBX cameras.",
                                                  "fbx import camera scene extras",
                                                  &FbxImportSettings::ImportCameras));
    assetImport.Fields.push_back(MakeFbxToggleRow("Lights", "Create light entities from imported FBX lights.",
                                                  "fbx import lights lamps scene extras",
                                                  &FbxImportSettings::ImportLights));
    assetImport.Fields.push_back(MakeFbxToggleRow(
        "Helper Nodes", "Preserve helper/null nodes from the authored FBX hierarchy.",
        "fbx import helpers empties null nodes scene extras", &FbxImportSettings::ImportHelperNodes));
    assetImport.Fields.push_back(MakeFbxToggleRow(
        "Generate Missing Tangents",
        "Generate tangent vectors for meshes that need normal mapping but do not provide tangents.",
        "fbx import generate missing tangents normal maps", &FbxImportSettings::GenerateMissingTangents));
    assetImport.Fields.push_back(MakeFbxToggleRow(
        "Clean Skin Weights", "Remove invalid, zero, and negative skin weights during FBX import.",
        "fbx import clean skin weights bones influences", &FbxImportSettings::CleanSkinWeights));
    assetImport.Fields.push_back(MakeFbxToggleRow("Adjust Pivots",
                                                  "Apply authored FBX pivot handling during import.",
                                                  "fbx import pivots", &FbxImportSettings::AdjustPivots));
    assetImport.Fields.push_back(MakeFbxToggleRow(
        "Preserve Geo Transforms", "Keep authored geometry transforms instead of baking them into mesh vertices.",
        "fbx import preserve geometry transforms", &FbxImportSettings::PreserveGeometryTransforms));
    assetImport.Fields.push_back(MakeFbxToggleRow(
        "Import Embedded Textures", "Read texture image data embedded directly in FBX files.",
        "fbx import embedded textures", &FbxImportSettings::ImportEmbeddedTextures));

    {
        SettingsFieldDescriptor autoLods;
        autoLods.Label = "Auto-Generate LODs";
        autoLods.Tooltip = "Auto-Generate LODs On Import: generate simplified mesh LODs automatically when a model "
                           "is imported into this project. When off, LODs can still be generated from the model "
                           "inspector or via the API.";
        autoLods.SearchKeywords = "lod level of detail auto generate import simplify";
        autoLods.SectionHeader = "Automatic LODs";
        SettingsFieldDescriptor::ToggleField toggle;
        toggle.DefaultValue = kDefaultAutoGenerateLods;
        toggle.Get = LoadAutoGenerateLods;
        toggle.Set = SaveAutoGenerateLods;
        autoLods.Control = std::move(toggle);
        assetImport.Fields.push_back(std::move(autoLods));
    }

    EditorSettingsRegistry::Get().RegisterCategory(std::move(assetImport));
}

} // namespace GameEngine::Editor
