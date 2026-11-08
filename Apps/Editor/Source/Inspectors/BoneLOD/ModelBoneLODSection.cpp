#include "Inspectors/BoneLOD/ModelBoneLODSection.h"

#include "Assets/AssetManager.h"
#include "Assets/BoneLOD/ModelBoneLOD.h"
#include "Assets/ModelAsset.h"
#include "Core/Engine.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Editor/EditorPaths.h"
#include "Inspectors/BoneLOD/BoneLODTreeProvider.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/ModelSettingsReload.h"
#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include "UndoRedo/SetAssetMetaValueCommand.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/TreeView.h"
#include "UI/Parsers/XMLParser.h"

#include <algorithm>
#include <limits>

namespace GameEngine::Editor
{

namespace
{
constexpr const char* kLayout = "UI/inspectors/BoneLOD/ModelBoneLODSection.uxml";
constexpr const char* kStyle = "UI/inspectors/BoneLOD/ModelBoneLODSection.css";

void WriteSettings(const std::filesystem::path& path, const std::string& metadata)
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    if (!assets.GetRegistry().SetMetaValue(path, BoneLODAssetSettings::kMetadataKey, metadata))
    {
        Logger::Log::Warning("Bone LOD settings could not be saved for {}.", path.string());
        return;
    }
    ReloadModelForChangedSettings(assets, assets.ResolveAssetGuid(path), path);
}
}

ModelBoneLODSection::ModelBoneLODSection(const ModelAsset& model, UndoRedoService* undo,
    std::function<void()> requestRefresh)
    : m_Path(model.GetPath()), m_Undo(undo), m_RequestRefresh(std::move(requestRefresh))
{
    AddClass("bone-lod-section");
    RequestSubtreeStyleAssetPath(kStyle, "editor");
    UniquePtr<UIElement> layout;
    if (!UIParsing::XMLParser::ParseLayoutFromFile((GetEditorGlobalPaths().installAssetsRoot / kLayout).string(), layout))
    {
        Logger::Log::Error("Bone LOD settings could not load {}. Rebuild the staged editor assets.", kLayout);
        return;
    }
    AddChild(std::move(layout));
    m_Tree = dynamic_cast<TreeView*>(FindById("BoneLODTree"));
    m_Status = dynamic_cast<Label*>(FindById("BoneLODStatus"));
    auto* summary = dynamic_cast<Label*>(FindById("BoneLODSummary"));
    auto* searchHost = FindById("BoneLODSearchHost");
    auto* fields = FindById("BoneLODFields");
    auto* clear = dynamic_cast<Button*>(FindById("BoneLODClear"));
    auto* reset = dynamic_cast<Button*>(FindById("BoneLODReset"));
    if (!m_Tree || !m_Status || !summary || !searchHost || !fields || !clear || !reset)
    {
        Logger::Log::Error("Bone LOD settings layout is missing its fields, tree, search, summary, status, or actions.");
        return;
    }
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const auto origin = assets.GetRegistry().GetImportSettingsOrigin(m_Path);
    m_Settings = BoneLODAssetSettings::Load(assets.GetRegistry(), m_Path);
    auto* budget = InspectorDrag::AddIntRowWithDrag(fields, "Bone budget",
        static_cast<int>(std::min(m_Settings.BoneBudget, static_cast<uint32>(std::numeric_limits<int>::max()))),
        [](int) {}, [this](int value) { SetBudget(value); }, static_cast<int>(BoneLODAssetSettings{}.BoneBudget),
        "Maximum skin-evaluation bones in the reduced tier. Required bones and their ancestors can exceed this budget. "
        "Zero keeps full detail. Add Bone LOD to a model instance to enable distance selection.");
    budget->SetRange(0, std::numeric_limits<int>::max());
    const auto& data = model.GetBoneLOD();
    if (data)
    {
        summary->SetText("Requested: " + std::to_string(data->RequestedBoneBudget()) +
            "   Required: " + std::to_string(data->RequiredBoneCount()) +
            "   Evaluated: " + std::to_string(data->ActualBoneCount()));
        m_Status->SetText(!data->Error().empty() ? data->Error() : data->Warning());
    }
    else
        summary->SetText("No skinned skeleton is available for reduction.");
    if (!m_Settings.Error.empty())
        m_Status->SetText(m_Settings.Error);
    if (!origin.Writable)
        m_Status->SetText("These bone settings belong to a read-only asset source. Edit the source package to change them.");
    fields->SetEnabled(origin.Writable && m_Settings.Error.empty());
    m_Tree->SetEnabled(origin.Writable && m_Settings.Error.empty());
    reset->SetEnabled(origin.Writable && !m_Settings.Error.empty());
    clear->SetEnabled(origin.Writable && m_Settings.Error.empty() && !m_Settings.MandatoryPaths.empty());
    reset->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ResetSettings(); });
    clear->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ClearMandatory(); });
    const auto* skeleton = Engine::Renderer::SkeletonStore::Instance().Get(model.GetSkeletonId());
    if (!skeleton)
        return;
    m_Provider = MakeUnique<BoneLODTreeProvider>(*skeleton);
    auto search = MakeUnique<SearchFieldWithFilter>();
    search->AddClass("bone-lod-search");
    search->SetFieldId("BoneLODSearch");
    search->SetPlaceholder("Filter bone names");
    m_Tree->SetDataProvider(m_Provider.get());
    m_Tree->SetVisibilityFilter([this](TreeId id) { return m_Provider->IsVisible(id); });
    m_Tree->SetOnRowBound([this](TreeId id, UIElement* row) { BindRow(id, row); });
    m_Tree->ExpandAll();
    const auto filter = [this](const std::string& value)
    {
        m_Provider->SetSearch(value);
        m_Tree->ExpandAll();
    };
    search->SetOnQueryChanging(filter);
    search->SetOnQueryChanged(filter);
    searchHost->AddChild(std::move(search));
}

ModelBoneLODSection::~ModelBoneLODSection()
{
    if (m_Tree)
        m_Tree->SetDataProvider(nullptr);
}

void ModelBoneLODSection::BindRow(uint64 id, UIElement* row)
{
    auto* checkbox = dynamic_cast<Checkbox*>(InspectorDrag::FindChildByClass(row, "bone-lod-pin"));
    if (!checkbox)
    {
        auto owned = MakeUnique<Checkbox>();
        owned->AddClass("bone-lod-pin");
        owned->SetText("Keep");
        owned->SetTooltip("Keep animation for this bone and its ancestors in the reduced tier.");
        checkbox = owned.get();
        row->AddChild(std::move(owned));
    }
    const auto path = m_Provider->Path(id);
    checkbox->SetValueWithoutNotify(std::find(m_Settings.MandatoryPaths.begin(), m_Settings.MandatoryPaths.end(), path)
        != m_Settings.MandatoryPaths.end());
    checkbox->SetOnValueChanged([this, id](bool value) { SetMandatory(id, value); });
    row->SetTooltip(Join(path, " / "));
}

void ModelBoneLODSection::SetMandatory(uint64 id, bool mandatory)
{
    auto path = m_Provider->Path(id);
    if (path.empty())
        return;
    std::erase(m_Settings.MandatoryPaths, path);
    if (mandatory)
        m_Settings.MandatoryPaths.push_back(std::move(path));
    Save();
}

void ModelBoneLODSection::SetBudget(int budget)
{
    m_Settings.BoneBudget = static_cast<uint32>(std::max(budget, 0));
    Save();
}

void ModelBoneLODSection::ClearMandatory()
{
    m_Settings.MandatoryPaths.clear();
    Save();
}

void ModelBoneLODSection::ResetSettings()
{
    m_Settings = BoneLODAssetSettings{};
    Save();
}

void ModelBoneLODSection::Save()
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    if (!assets.GetRegistry().GetImportSettingsOrigin(m_Path).Writable)
        return;
    const auto metadata = m_Settings.Encode();
    if (!metadata)
    {
        m_Status->SetText("Bone LOD settings could not be saved. Check the asset registry and try again.");
        return;
    }
    std::string stored;
    if (assets.GetRegistry().TryGetMetaValue(m_Path, BoneLODAssetSettings::kMetadataKey, stored) && stored == *metadata)
        return;
    if (m_Undo)
    {
        m_Undo->Execute(MakeUnique<SetAssetMetaValueCommand>(m_Path, BoneLODAssetSettings::kMetadataKey,
            *metadata, WriteSettings, m_RequestRefresh));
        return;
    }
    WriteSettings(m_Path, *metadata);
    if (m_RequestRefresh)
        m_RequestRefresh();
}

} // namespace GameEngine::Editor
