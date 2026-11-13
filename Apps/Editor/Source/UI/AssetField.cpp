#include "UI/AssetField.h"

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetSearchProvider.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "Editor/DragDropPayloads.h"
#include "Types/StringUtils.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/Interaction/Payload.h"

#include <any>
#include <filesystem>
#include <format>
#include <string_view>

namespace GameEngine
{
namespace
{
constexpr const char* kMissingAssetText = "Missing asset";
// The model thumbnail's requested edge, in pixels.
constexpr int kModelThumbnailSize = 64;

// Display name for a GUID that names an engine built-in rather than a registry
// asset. Meshes first, then materials — the two families are separate lookups
// because IsPrimitive() means "primitive mesh".
std::string_view BuiltInAssetName(const GUID& guid)
{
    using Engine::Renderer::PrimitiveGenerator;
    if (const std::string_view mesh = PrimitiveGenerator::NameFromGuid(guid); !mesh.empty())
        return mesh;
    return PrimitiveGenerator::MaterialNameFromGuid(guid);
}

// The name's tooltip for a registry asset: its file name as spelled on disk, then its GUID.
// The registry path is a lookup key, case-folded on a case-insensitive volume: do not show it.
std::string ResolvedAssetTooltip(const AssetMetadata& meta)
{
    return std::format("{}{}\nGUID {}", meta.Name, meta.Extension, meta.Guid.ToString());
}
} // namespace

AssetField::AssetField()
{
    // The base builds the parts from the field's layout; the icon and the preview
    // strip lead them, both hidden until a value shows them.
    m_Icon = LayoutPart("asset-field-icon");
    m_Preview = LayoutPart("asset-field-preview");
}

void AssetField::SetAcceptedTypes(std::vector<AssetType> types)
{
    m_AcceptedTypes = std::move(types);
}

void AssetField::SetMetadataFilter(std::function<bool(const AssetIndexRecord&)> filter)
{
    m_MetadataFilter = std::move(filter);
}

void AssetField::SetAssetRegistry(AssetRegistry* registry)
{
    m_Registry = registry;
}

void AssetField::SetValue(const GUID& guid)
{
    if (m_Value == guid && !ResolutionChanged())
        return;
    m_Value = guid;
    UpdateDisplay();
}

bool AssetField::ResolutionChanged() const
{
    if (m_Value.IsNull())
        return false;
    AssetMetadata meta;
    if (!m_Registry || !m_Registry->TryGetAssetMetadata(m_Value, meta))
        return !m_ResolvedPath.empty();
    return meta.Path != m_ResolvedPath || meta.Name != m_ResolvedName;
}

void AssetField::Clear()
{
    ApplyValue(GUID::Null());
}

void AssetField::ApplyValue(const GUID& guid)
{
    if (m_Value == guid)
        return;
    m_Value = guid;
    UpdateDisplay();
    if (m_OnValueChanged)
        m_OnValueChanged(m_Value);
}

void AssetField::ClearModelPreviewStrip()
{
    if (!m_Preview)
        return;
    m_Preview->AddClass("hidden");
    UI::Layout::ClearBackgroundOverride(*m_Preview);
}

void AssetField::SetIconClass(std::string iconClass)
{
    if (!m_Icon)
        return;
    if (!m_CurrentIconClass.empty())
        m_Icon->RemoveClass(m_CurrentIconClass);
    m_CurrentIconClass = std::move(iconClass);
    if (m_CurrentIconClass.empty())
    {
        m_Icon->AddClass("hidden");
        return;
    }
    m_Icon->AddClass(m_CurrentIconClass);
    m_Icon->RemoveClass("hidden");
}

void AssetField::UpdateDisplay()
{
    m_ResolvedPath.clear();
    m_ResolvedName.clear();
    if (m_Value.IsNull())
    {
        ClearModelPreviewStrip();
        SetIconClass({});
        ShowEmpty();
        return;
    }

    AssetMetadata meta;
    if (m_Registry && m_Registry->TryGetAssetMetadata(m_Value, meta))
    {
        m_ResolvedPath = meta.Path;
        m_ResolvedName = meta.Name;
        ShowNamed(meta.Name, ResolvedAssetTooltip(meta));
        SetIconClass(AssetSearchProvider::GetIconClassForType(meta.Type));

        if (meta.Type == AssetType::Texture && m_Preview && !meta.Path.empty())
        {
            // A texture is its own thumbnail: the strip draws the file directly, and the
            // UI texture cache shares it with larger previews of the same file.
            ++m_ModelPreviewRequestGen; // invalidate any in-flight model thumb
            UI::Layout::SetBackgroundPath(*m_Preview, meta.Path.string());
            m_Preview->RemoveClass("hidden");
            SetIconClass({});
        }
        else if (meta.Type == AssetType::Model && m_Thumbnails && m_Preview)
        {
            ++m_ModelPreviewRequestGen;
            const uint32_t requestGeneration = m_ModelPreviewRequestGen;
            ClearModelPreviewStrip();

            // GetOrRequest always fires the callback synchronously (via the
            // model-handler or fallback path), so act only through the callback:
            // acting on the return value as well would apply the thumbnail twice.
            m_Thumbnails->GetOrRequest(
                meta.Path, kModelThumbnailSize,
                [this, requestGeneration](const std::string& thumbnailPath) {
                    ApplyModelThumbnail(requestGeneration, thumbnailPath);
                }, true);
        }
        else
        {
            ClearModelPreviewStrip();
        }
        return;
    }

    ClearModelPreviewStrip();
    SetIconClass({});

    // Engine built-ins — the default scene's primitive meshes and materials — are
    // deliberately not registry assets, so the metadata lookup above misses them
    // by design. Name them anyway: built-ins are what a fresh scene is made of.
    if (const std::string_view builtIn = BuiltInAssetName(m_Value); !builtIn.empty())
    {
        ShowNamed(std::string(builtIn), std::format("Engine built-in\nGUID {}", m_Value.ToString()));
        return;
    }

    ShowMissing(kMissingAssetText,
                std::format("No asset has the GUID {}. Choose another asset or clear the field.", m_Value.ToString()));
}

void AssetField::ApplyModelThumbnail(uint32_t requestGeneration, const std::string& thumbnailPath)
{
    if (requestGeneration != m_ModelPreviewRequestGen || !m_Preview)
        return;
    if (thumbnailPath.empty())
    {
        ClearModelPreviewStrip();
        SetIconClass(AssetSearchProvider::GetIconClassForType(AssetType::Model));
        return;
    }
    constexpr std::string_view kEnginePrefix = "engine:";
    if (thumbnailPath.starts_with(kEnginePrefix))
        UI::Layout::SetBackgroundResourceName(*m_Preview, thumbnailPath.substr(kEnginePrefix.size()));
    else
        UI::Layout::SetBackgroundPath(*m_Preview, thumbnailPath);
    m_Preview->RemoveClass("hidden");
    SetIconClass({});
}

void AssetField::SetOnReveal(RevealCallback cb)
{
    m_OnReveal = std::move(cb);
    RefreshLinkStyle();
}

bool AssetField::CanReveal() const
{
    return m_OnReveal && !m_ResolvedPath.empty();
}

void AssetField::Reveal()
{
    m_OnReveal(m_ResolvedPath);
}

std::unique_ptr<ISearchProvider> AssetField::CreateProvider()
{
    if (!m_Registry)
        return nullptr;

    auto provider = std::make_unique<AssetSearchProvider>(m_Registry);
    if (!m_AcceptedTypes.empty())
        provider->SetTypeFilter(m_AcceptedTypes);
    if (m_MetadataFilter)
        provider->SetMetadataFilter(m_MetadataFilter);
    provider->SetShowFullPath(true);
    if (m_Thumbnails)
        provider->SetThumbnailProvider(m_Thumbnails);
    return provider;
}

void AssetField::OnResultSelected(const SearchResultItem& item)
{
    if (const auto* guid = std::any_cast<GUID>(&item.UserData))
        ApplyValue(*guid);
}

SearchItemId AssetField::CurrentResultId() const
{
    return AssetSearchProvider::ResultIdFor(m_Value);
}

void AssetField::PositionDialog(SearchDialog& dialog)
{
    // Anchor the panel's right edge to the field's right edge (Inspector fields sit
    // on the window edge, so the panel extends left).
    dialog.SetAnchorPosition(GetLayoutX() + GetLayoutWidth(), GetLayoutY() + GetLayoutHeight(),
                             SearchDialogHorizontalAnchor::TrailingRight);
}

bool AssetField::IsAcceptedExtension(const std::string& ext) const
{
    if (m_AcceptedTypes.empty())
        return true; // Accept any asset type

    for (auto type : m_AcceptedTypes)
    {
        if (IsValidExtensionForAssetType(type, ext))
            return true;
    }
    return false;
}

// -- IDropTarget implementation ------------------------------------------------

bool AssetField::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>();
}

UI::Interaction::DropFeedback AssetField::CanDrop(const UI::Interaction::DropRequest& request) const
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload || payload->paths.empty())
        return {false, "No payload"};

    const std::filesystem::path p = payload->paths[0];
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec))
        return {false, "Missing file"};
    if (std::filesystem::is_directory(p, ec))
        return {false, "Drop a file, not a folder"};

    const std::string ext = p.extension().string();
    if (!IsAcceptedExtension(ext))
    {
        std::string typeNames;
        for (auto type : m_AcceptedTypes)
        {
            if (!typeNames.empty())
                typeNames += "/";
            typeNames += AssetTypeToString(type);
        }
        return {false, "Expected " + typeNames + " asset"};
    }
    if (m_MetadataFilter && m_Registry)
    {
        AssetMetadata meta{};
        if (!m_Registry->TryGetAssetMetadata(p, meta) ||
            !m_MetadataFilter(ToAssetIndexRecord(meta)))
            return {false, "Asset is not valid for this slot"};
    }

    return {true, {}};
}

void AssetField::PerformDrop(const UI::Interaction::DropRequest& request)
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload || payload->paths.empty() || !m_Registry)
        return;

    const std::filesystem::path p = payload->paths[0];
    const auto result = m_Registry->RegisterAssetByPath(p);
    if (!result.IsOk())
        return;

    if (m_MetadataFilter)
    {
        AssetMetadata meta{};
        if (!m_Registry->TryGetAssetMetadata(result.Value(), meta) ||
            !m_MetadataFilter(ToAssetIndexRecord(meta)))
            return;
    }

    ApplyValue(result.Value());
}

} // namespace GameEngine

namespace RegisterAssetField
{
static auto s_RegisterAssetField = GameEngine::UIRegistration::RegisterWithFactory<GameEngine::AssetField>(
    "AssetField", []() { return std::make_unique<GameEngine::AssetField>(); });
}
