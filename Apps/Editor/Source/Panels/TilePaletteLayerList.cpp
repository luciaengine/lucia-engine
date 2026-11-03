#include "Panels/TilePaletteLayerList.h"

#include "Editor/DragDropPayloads.h"
#include "EditorContext.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "TileMap/TileLayerCommands.h"
#include "TileMap/TileMapEditContext.h"
#include "UI/EditorIcons.h"
#include "UI/Interaction/DragDropManager.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/InputSystem.h"

#include "Components/TileMap/TileMapLayerRef.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "TileMap/TileMapData.h"
#include "TileMap/TileMapLayerData.h"
#include "TileMapECS/TileMapRuntimeStore.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine
{

using Editor::TileLayerEdits::LayerRestoreData;
using Editor::TileLayerEdits::LayerStateSnapshot;

namespace
{
constexpr uint32 kCmdLayerDelete = 0x7101;
constexpr uint32 kCmdLayerDuplicate = 0x7102;
constexpr uint32 kCmdLayerLockToggle = 0x7103;
constexpr uint32 kCmdSelectedShow = 0x7104;
constexpr uint32 kCmdSelectedHide = 0x7105;
constexpr uint32 kCmdSelectedLock = 0x7106;
constexpr uint32 kCmdSelectedUnlock = 0x7107;
constexpr uint32 kCmdSelectedDelete = 0x7108;
// A press moves this far before it is a reorder drag rather than a click.
constexpr float32 kDragThresholdPx = 4.0f;

std::unique_ptr<Label> MakeLabel(const std::string& cssClass, const std::string& text)
{
    auto label = std::make_unique<Label>();
    label->AddClass(cssClass);
    label->SetText(text);
    return label;
}

/** A labelled action button with its glyph inset at the left. */
std::unique_ptr<Button> MakeSmallButton(const std::string& id, const std::string& text,
                                        const std::string& tooltip, const std::string& glyphClass)
{
    auto button = std::make_unique<Button>();
    button->SetId(id);
    button->SetText(text);
    button->AddClass("small");
    button->AddClass("secondary");
    button->AddClass("tm-icon-label");
    button->AddClass(glyphClass);
    button->SetTooltip(tooltip);
    return button;
}

/** A glyph-only button. The tooltip is the only place its name appears. */
std::unique_ptr<Button> MakeIconButton(const std::string& id, const std::string& tooltip,
                                       const std::string& glyphClass)
{
    auto button = std::make_unique<Button>();
    button->SetId(id);
    button->AddClass("small");
    button->AddClass("secondary");
    button->AddClass("tm-icon");
    button->AddClass(glyphClass);
    button->SetTooltip(tooltip);
    return button;
}
} // namespace

TilePaletteLayerList::TilePaletteLayerList()
{
    AddClass("tp-layers");
    BuildHeader();

    auto list = std::make_unique<ScrollView>();
    list->AddClass("tp-layer-list");
    m_Rows = list->GetViewport();
    // The rows are the viewport's children, so the gap between bars has to be
    // styled here -- padding on the ScrollView insets them, but never spaces them.
    if (m_Rows != nullptr)
        m_Rows->AddClass("tp-layer-list-viewport");
    AddChild(std::move(list));
}

void TilePaletteLayerList::SetOnLayerTargeted(std::function<void()> callback)
{
    m_OnLayerTargeted = std::move(callback);
}

void TilePaletteLayerList::SetOnLayerAdded(std::function<void()> callback)
{
    m_OnLayerAdded = std::move(callback);
}

void TilePaletteLayerList::SetOnCreateMapRequested(std::function<void()> callback)
{
    m_OnCreateMapRequested = std::move(callback);
}

void TilePaletteLayerList::SetOnHint(std::function<void(const std::string&)> callback)
{
    m_OnHint = std::move(callback);
}

void TilePaletteLayerList::Hint(const std::string& text)
{
    if (m_OnHint)
        m_OnHint(text);
}

void TilePaletteLayerList::BuildHeader()
{
    auto header = std::make_unique<UIElement>();
    header->AddClass("tp-row");
    header->AddChild(MakeLabel("tp-row-label", "Layers"));

    auto add = MakeSmallButton("TilePaletteAddLayer", "Add", "Add a tile layer to this tilemap",
                               "tm-glyph-add-layer");
    add->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { AddLayer(/*intGrid=*/false); });
    header->AddChild(std::move(add));

    auto addIntGrid = MakeSmallButton("TilePaletteAddIntGridLayer", "Add IntGrid",
                                      "Add an IntGrid layer to this tilemap",
                                      "tm-glyph-add-intgrid-layer");
    addIntGrid->RegisterEventHandler(kEventButtonClick,
                                     [this](UIEvent&) { AddLayer(/*intGrid=*/true); });
    header->AddChild(std::move(addIntGrid));

    auto remove = MakeSmallButton("TilePaletteRemoveLayer", "Remove",
                                  "Remove the active layer from this tilemap", "tm-glyph-remove");
    remove->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { RemoveActiveLayer(); });
    header->AddChild(std::move(remove));

    auto newMap = MakeSmallButton("TilePaletteNewMap", "New Map",
                                  "Create a new .tilemap asset in the project", "tm-glyph-new-map");
    newMap->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_OnCreateMapRequested)
            m_OnCreateMapRequested();
    });
    header->AddChild(std::move(newMap));

    auto save = MakeSmallButton("TilePaletteSaveMap", "Save",
                                "Write painted cells and layer changes back to the .tilemap asset",
                                "tm-glyph-save");
    save->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
        if (store == nullptr || m_ListedMap.IsNull())
            return;
        Hint(store->SaveMap(m_ListedMap) ? "Saved tilemap." : "Failed to save tilemap (see log).");
    });
    header->AddChild(std::move(save));

    AddChild(std::move(header));
}

void TilePaletteLayerList::TargetLayer(ECS::EntityHandle entity, uint32 layerId, int modifiers)
{
    if (m_EditContext == nullptr)
        return;

    const bool toggle = (modifiers & Input::kModControl) != 0 || (modifiers & Input::kModSuper) != 0;
    const bool extend = (modifiers & Input::kModShift) != 0;
    const uint32 previousTarget = m_EditContext->TargetLayer();
    if (toggle)
    {
        const auto it = std::find(m_Selected.begin(), m_Selected.end(), layerId);
        if (it != m_Selected.end() && m_Selected.size() > 1)
            m_Selected.erase(it);
        else if (it == m_Selected.end())
            m_Selected.push_back(layerId);
    }
    else if (extend && previousTarget != 0)
    {
        // A range in the order shown, the way every list treats Shift.
        const std::vector<uint32> order = RowOrder();
        const auto from = std::find(order.begin(), order.end(), previousTarget);
        const auto to = std::find(order.begin(), order.end(), layerId);
        if (from != order.end() && to != order.end())
        {
            const auto first = std::min(from, to);
            const auto last = std::max(from, to);
            for (auto it = first; it <= last; ++it)
            {
                if (std::find(m_Selected.begin(), m_Selected.end(), *it) == m_Selected.end())
                    m_Selected.push_back(*it);
            }
        }
    }
    else
    {
        m_Selected.assign(1, layerId);
    }
    ApplySelectionClasses();

    m_EditContext->SetTarget(entity, m_ListedMap, layerId);
    m_ListedTarget = layerId;
    m_PendingActiveLayer = layerId;
    m_ActiveRowDirty = true;
    if (m_OnLayerTargeted)
        m_OnLayerTargeted();
}

std::vector<uint32> TilePaletteLayerList::BatchLayers(uint32 layerId) const
{
    if (m_Selected.size() > 1 &&
        std::find(m_Selected.begin(), m_Selected.end(), layerId) != m_Selected.end())
        return m_Selected;
    return {layerId};
}

void TilePaletteLayerList::ApplySelectionClasses()
{
    for (const auto& [rowLayer, row] : m_RowElements)
    {
        if (row == nullptr)
            continue;
        const bool selected = m_Selected.size() > 1 &&
                              std::find(m_Selected.begin(), m_Selected.end(), rowLayer) !=
                                  m_Selected.end();
        if (selected == row->HasClass("selected"))
            continue;
        if (selected)
            row->AddClass("selected");
        else
            row->RemoveClass("selected");
    }
}

void TilePaletteLayerList::SetSelectedVisible(const std::vector<uint32>& layers, bool visible)
{
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (world == nullptr || store == nullptr || m_ListedMap.IsNull())
        return;
    const LayerStateSnapshot before = CaptureState();
    bool changed = false;
    for (uint32 layerId : layers)
    {
        const ECS::EntityHandle entity =
            Editor::TileLayerEdits::FindLayerEntity(*world, m_ListedMap, layerId);
        if (!entity.IsValid())
            continue;
        if (auto* ref = world->GetComponentForWrite<Components::TileMapLayerRef>(entity);
            ref != nullptr && ref->Visible != visible)
        {
            ref->Visible = visible;
            changed = true;
        }
    }
    if (!changed)
        return;
    store->MarkStructuralChange(m_ListedMap);
    CommitLayerEdit(visible ? "Show Layers" : "Hide Layers", before);
}

void TilePaletteLayerList::SetSelectedLocked(const std::vector<uint32>& layers, bool locked)
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    TileMap::TileMapData* map = store != nullptr && !m_ListedMap.IsNull() ? store->GetMap(m_ListedMap) : nullptr;
    if (map == nullptr)
        return;
    const LayerStateSnapshot before = CaptureState();
    bool changed = false;
    for (uint32 layerId : layers)
    {
        if (TileMap::TileMapLayerData* layer = map->FindLayer(layerId);
            layer != nullptr && layer->Locked != locked)
        {
            layer->Locked = locked;
            changed = true;
        }
    }
    if (!changed)
        return;
    store->MarkStructuralChange(m_ListedMap);
    CommitLayerEdit(locked ? "Lock Layers" : "Unlock Layers", before);
}

void TilePaletteLayerList::DeleteLayers(const std::vector<uint32>& layers)
{
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (world == nullptr || store == nullptr || m_ListedMap.IsNull())
        return;
    // One undo entry per layer, committed in order; Ctrl+Z walks them back one
    // at a time, which is how the undo stack lists them too.
    for (uint32 layerId : layers)
    {
        LayerRestoreData restore;
        const bool captured = Editor::TileLayerEdits::CaptureLayerForRestore(
            *world, *store, m_ListedMap, layerId, restore);
        Editor::TileLayerEdits::DestroyLayerAndEntity(*world, *store, m_ListedMap, layerId);
        if (captured && m_UndoRedo != nullptr)
        {
            m_UndoRedo->CommitAlreadyApplied(std::make_unique<Editor::TileLayerExistenceCommand>(
                "Delete Layer", /*createsOnDo*/ false, std::move(restore)));
        }
    }
    m_Selected.clear();
    if (m_EditContext != nullptr &&
        std::find(layers.begin(), layers.end(), m_EditContext->TargetLayer()) != layers.end())
        m_EditContext->SetTarget(ECS::EntityHandle{}, m_ListedMap, 0);
}

void TilePaletteLayerList::AddLayer(bool intGrid)
{
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    if (m_EditContext == nullptr || store == nullptr || world == nullptr || m_ListedMap.IsNull())
        return;
    TileMap::TileMapData* data = store->GetMap(m_ListedMap);
    if (data == nullptr)
        return;

    const std::string name =
        (intGrid ? "IntGrid " : "Layer ") + std::to_string(data->Layers().size() + 1);
    const uint32 layerId = data->AddLayer(
        name, intGrid ? TileMap::TileMapLayerType::IntGrid : TileMap::TileMapLayerType::Tiles);
    if (layerId == 0)
        return;
    if (intGrid)
    {
        // An IntGrid layer with no values is unpaintable, so a fresh one starts
        // with the value every semantic layer ends up having: something solid.
        if (TileMap::TileMapLayerData* layer = data->FindLayer(layerId))
            layer->IntGridValues.push_back(TileMap::IntGridValueDef{1, "Value 1", 0x4FA3FFFFu});
    }
    const ECS::EntityHandle entity = Editor::TileLayerEdits::CreateLayerEntity(
        *world, m_ListedMap, layerId, name, m_EditContext->TargetEntity());
    store->MarkStructuralChange(m_ListedMap);

    if (m_UndoRedo != nullptr)
    {
        LayerRestoreData restore;
        if (Editor::TileLayerEdits::CaptureLayerForRestore(*world, *store, m_ListedMap, layerId,
                                                           restore))
        {
            m_UndoRedo->CommitAlreadyApplied(std::make_unique<Editor::TileLayerExistenceCommand>(
                intGrid ? "Add IntGrid Layer" : "Add Layer", /*createsOnDo*/ true,
                std::move(restore)));
        }
    }

    TargetLayer(entity, layerId, /*modifiers=*/0);
    if (!intGrid && m_OnLayerAdded)
        m_OnLayerAdded();
}

void TilePaletteLayerList::RemoveActiveLayer()
{
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    if (m_EditContext == nullptr || store == nullptr || m_ListedMap.IsNull())
        return;
    TileMap::TileMapData* data = store->GetMap(m_ListedMap);
    if (data == nullptr)
        return;

    const uint32 layerId = m_EditContext->TargetLayer();
    const std::vector<TileMap::TileMapLayerData>& layers = data->Layers();
    const auto it = std::find_if(layers.begin(), layers.end(),
                                 [layerId](const TileMap::TileMapLayerData& layer)
                                 { return layer.Id == layerId; });
    if (it == layers.end())
        return;

    // Deleting a layer destroys every cell painted on it. Without the undo stack
    // that is unrecoverable, so the button refuses rather than doing it anyway —
    // an unwired service is a bug to fix, not a reason to risk the author's work.
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    if (m_UndoRedo == nullptr || world == nullptr)
    {
        Hint("Cannot remove the layer: no undo service, and this would not be recoverable.");
        return;
    }

    const std::string name = it->Name;
    const std::vector<uint32> batch = BatchLayers(layerId);
    DeleteLayers(batch);
    Hint(batch.size() > 1 ? "Removed " + std::to_string(batch.size()) + " layers — Ctrl+Z restores them one by one."
                          : "Removed layer \"" + name + "\" — Ctrl+Z restores it.");
}

void TilePaletteLayerList::DuplicateLayer(uint32 sourceLayer)
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    if (store == nullptr || world == nullptr || m_ListedMap.IsNull())
        return;
    TileMap::TileMapData* map = store->GetMap(m_ListedMap);
    if (map == nullptr)
        return;
    const TileMap::TileMapLayerData* source = map->FindLayer(sourceLayer);
    if (source == nullptr)
        return;

    const std::string copyName =
        (source->Name.empty() ? std::string("Layer") : source->Name) + " Copy";
    const uint32 copyId = map->DuplicateLayer(sourceLayer, copyName);
    if (copyId == 0)
        return;

    // Clone the entity that draws it, nudged just in front. Copying only the
    // asset layer would add a layer nothing in the scene renders.
    Editor::TileLayerEdits::CreateLayerEntity(
        *world, m_ListedMap, copyId, copyName,
        Editor::TileLayerEdits::FindLayerEntity(*world, m_ListedMap, sourceLayer));
    store->MarkStructuralChange(m_ListedMap);

    if (m_UndoRedo != nullptr)
    {
        LayerRestoreData restore;
        if (Editor::TileLayerEdits::CaptureLayerForRestore(*world, *store, m_ListedMap, copyId,
                                                           restore))
        {
            m_UndoRedo->CommitAlreadyApplied(std::make_unique<Editor::TileLayerExistenceCommand>(
                "Duplicate Layer", /*createsOnDo*/ true, std::move(restore)));
        }
    }
}

LayerStateSnapshot TilePaletteLayerList::CaptureState() const
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    if (store == nullptr || world == nullptr)
        return LayerStateSnapshot{};
    return Editor::TileLayerEdits::CaptureLayerState(*world, *store, m_ListedMap);
}

void TilePaletteLayerList::CommitLayerEdit(const char* name, const LayerStateSnapshot& before)
{
    if (m_UndoRedo == nullptr || before.Entries.empty())
        return;
    LayerStateSnapshot after = CaptureState();
    if (after.Entries.size() != before.Entries.size())
        return;
    // Nothing moved: pushing anyway would put empty steps on the stack that undo
    // to no visible change.
    if (!after.DiffersFrom(before))
        return;
    m_UndoRedo->CommitAlreadyApplied(
        std::make_unique<Editor::TileLayerStateCommand>(name, before, std::move(after)));
}

std::vector<uint32> TilePaletteLayerList::RowOrder() const
{
    std::vector<uint32> order;
    order.reserve(m_RowElements.size());
    for (const auto& [layerId, row] : m_RowElements)
        order.push_back(layerId);
    return order;
}

void TilePaletteLayerList::ShowRowMenu(uint32 layerId, int x, int y)
{
    if (m_Context == nullptr || m_Context->MainWindow == nullptr)
        return;

    if (!m_RowMenu)
    {
        m_RowMenu = CreateContextMenu();
        if (!m_RowMenu)
            return;
        m_RowMenu->SetCommandHandler([this](uint32_t command)
        {
            auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
            ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
            if (store == nullptr || world == nullptr || m_ListedMap.IsNull())
                return;
            TileMap::TileMapData* map = store->GetMap(m_ListedMap);
            if (map == nullptr)
                return;
            const uint32 layer = m_RowMenuLayer;

            switch (command)
            {
            case kCmdLayerDelete:
                DeleteLayers({layer});
                break;
            case kCmdSelectedDelete:
                DeleteLayers(BatchLayers(layer));
                break;
            case kCmdSelectedShow:
                SetSelectedVisible(BatchLayers(layer), true);
                break;
            case kCmdSelectedHide:
                SetSelectedVisible(BatchLayers(layer), false);
                break;
            case kCmdSelectedLock:
                SetSelectedLocked(BatchLayers(layer), true);
                break;
            case kCmdSelectedUnlock:
                SetSelectedLocked(BatchLayers(layer), false);
                break;

            case kCmdLayerDuplicate:
                DuplicateLayer(layer);
                break;
            case kCmdLayerLockToggle:
                if (TileMap::TileMapLayerData* target = map->FindLayer(layer))
                {
                    const LayerStateSnapshot before = CaptureState();
                    target->Locked = !target->Locked;
                    store->MarkStructuralChange(m_ListedMap);
                    CommitLayerEdit("Lock Layer", before);
                }
                break;
            default:
                break;
            }
        });
    }

    m_RowMenuLayer = layerId;
    m_RowMenu->Clear();

    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    const TileMap::TileMapData* map =
        (store != nullptr && !m_ListedMap.IsNull()) ? store->GetMap(m_ListedMap) : nullptr;
    const TileMap::TileMapLayerData* layer = map ? map->FindLayer(layerId) : nullptr;
    const bool locked = layer != nullptr && layer->Locked;

    ContextMenuBuilder builder;
    builder.AddItem(locked ? "Unlock Layer" : "Lock Layer", kCmdLayerLockToggle, MenuItemFlag_None, 0,
                    locked ? EditorIcons::kLockOpen : EditorIcons::kLockClosed);
    builder.AddItem("Duplicate Layer", kCmdLayerDuplicate, MenuItemFlag_None, 0, EditorIcons::kCopy);
    builder.AddItem("---1", 0, MenuItemFlag_None, 0);
    builder.AddItem("Delete Layer", kCmdLayerDelete, MenuItemFlag_None, 0, EditorIcons::kTrash);
    // With several rows marked (Ctrl/Shift-click), the same verbs apply to all
    // of them; the single-row items above stay so a right-click on one row of
    // the set can still act on that row alone.
    const std::vector<uint32> batch = BatchLayers(layerId);
    if (batch.size() > 1)
    {
        const std::string count = std::to_string(batch.size());
        builder.AddItem("---2", 0, MenuItemFlag_None, 0);
        builder.AddItem("Show " + count + " Selected", kCmdSelectedShow, MenuItemFlag_None, 0);
        builder.AddItem("Hide " + count + " Selected", kCmdSelectedHide, MenuItemFlag_None, 0);
        builder.AddItem("Lock " + count + " Selected", kCmdSelectedLock, MenuItemFlag_None, 0,
                        EditorIcons::kLockClosed);
        builder.AddItem("Unlock " + count + " Selected", kCmdSelectedUnlock, MenuItemFlag_None, 0,
                        EditorIcons::kLockOpen);
        builder.AddItem("Delete " + count + " Selected", kCmdSelectedDelete, MenuItemFlag_None, 0,
                        EditorIcons::kTrash);
    }
    // Authored order, not the builder's default priority-then-name sort: equal
    // priorities would sort these alphabetically and put Delete at the top, ahead
    // of the separator meant to hold it apart from the harmless entries.
    builder.Build(m_RowMenu.get(), /*sortByPriorityThenPath*/ false);
    m_RowMenu->Show(m_Context->MainWindow, x, y);
}

bool TilePaletteLayerList::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::TilePaletteLayerReorderPayload>();
}

bool TilePaletteLayerList::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    if (m_Rows == nullptr)
        return false;
    // Rows are listed frontmost-first, so "above" means nearer the camera. The
    // insertion point is the half of the row the pointer is in.
    for (const auto& [layerId, row] : m_RowElements)
    {
        if (row == nullptr)
            continue;
        const float top = row->GetLayoutY();
        const float height = row->GetLayoutHeight();
        if (height <= 0.0f || y < top || y >= top + height)
            continue;
        if (x < row->GetLayoutX() || x >= row->GetLayoutX() + row->GetLayoutWidth())
            continue;
        out.TargetId = layerId;
        out.Location = (y < top + height * 0.5f) ? UI::Interaction::DropLocation::BeforeItem
                                                 : UI::Interaction::DropLocation::AfterItem;
        return true;
    }
    return false;
}

UI::Interaction::DropFeedback TilePaletteLayerList::CanDrop(
    const UI::Interaction::DropRequest& request) const
{
    const auto* payload = request.payload.TryGet<Editor::TilePaletteLayerReorderPayload>();
    if (payload == nullptr)
        return {false, "Not a layer"};
    if (static_cast<uint32>(request.hit.TargetId) == payload->layerId)
        return {false, "Same layer"};
    return {true, ""};
}

void TilePaletteLayerList::PerformDrop(const UI::Interaction::DropRequest& request)
{
    const auto* payload = request.payload.TryGet<Editor::TilePaletteLayerReorderPayload>();
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    if (payload == nullptr || store == nullptr || world == nullptr)
        return;
    // The drag reordered live, so the undo baseline is the state captured when
    // it began, not the state a moment before the drop.
    LayerStateSnapshot before = CaptureState();
    for (LayerStateSnapshot::Entry& entry : before.Entries)
    {
        for (const auto& [layerId, z] : m_DragRestore)
        {
            if (layerId == entry.LayerId)
                entry.ZOffset = z;
        }
    }
    Editor::TileLayerEdits::ApplyLayerOrder(
        *world, *store, m_ListedMap, RowOrder(), payload->layerId,
        static_cast<uint32>(request.hit.TargetId),
        request.hit.Location == UI::Interaction::DropLocation::AfterItem);
    m_DragRestore.clear();
    CommitLayerEdit("Reorder Layer", before);
}

void TilePaletteLayerList::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    auto* store = TileMapECS::TileMapRuntimeStore::TryGet();
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    if (store == nullptr || world == nullptr)
        return;

    // First preview of a drag: remember every layer's depth so a cancelled drag
    // puts the map back exactly as it was.
    if (state.Visible && m_DragRestore.empty())
    {
        for (const auto& [layerId, row] : m_RowElements)
        {
            const ECS::EntityHandle entity =
                Editor::TileLayerEdits::FindLayerEntity(*world, m_ListedMap, layerId);
            if (const auto* ref = entity.IsValid()
                                      ? world->GetComponent<Components::TileMapLayerRef>(entity)
                                      : nullptr)
            {
                m_DragRestore.emplace_back(layerId, ref->ZOffset);
            }
        }
    }

    if (!state.Visible)
    {
        // Drag left the list or was cancelled: restore, rather than leaving the
        // map in whatever arrangement the pointer last passed over.
        bool restored = false;
        for (const auto& [layerId, z] : m_DragRestore)
        {
            const ECS::EntityHandle entity =
                Editor::TileLayerEdits::FindLayerEntity(*world, m_ListedMap, layerId);
            if (!entity.IsValid())
                continue;
            if (auto* ref = world->GetComponentForWrite<Components::TileMapLayerRef>(entity))
            {
                ref->ZOffset = z;
                restored = true;
            }
        }
        m_DragRestore.clear();
        if (restored)
            store->MarkStructuralChange(m_ListedMap);
        return;
    }

    if (!state.Allowed || m_DragLayer == 0)
        return;
    // Reorder for real as the pointer moves: the rows and the scene both follow
    // the drag, which answers "where will this land" better than a line would.
    Editor::TileLayerEdits::ApplyLayerOrder(
        *world, *store, m_ListedMap, RowOrder(), m_DragLayer,
        static_cast<uint32>(state.Hit.TargetId),
        state.Hit.Location == UI::Interaction::DropLocation::AfterItem);
}

void TilePaletteLayerList::Update()
{
    if (m_RowsDirty)
    {
        m_RowsDirty = false;
        RebuildRows();
    }
    if (m_ActiveRowDirty)
    {
        m_ActiveRowDirty = false;
        SetActiveRow(m_PendingActiveLayer);
    }
    RefreshRows();
}

void TilePaletteLayerList::SetActiveRow(uint32 layerId)
{
    for (const auto& [rowLayer, row] : m_RowElements)
    {
        if (row == nullptr)
            continue;
        if (rowLayer == layerId)
            row->AddClass("active");
        else
            row->RemoveClass("active");
    }
}

void TilePaletteLayerList::RefreshRows()
{
    if (m_EditContext == nullptr)
        return;
    const ECS::EntityHandle entity = m_EditContext->TargetEntity();
    const GUID mapGuid = m_EditContext->TargetMap();
    const uint32 target = m_EditContext->TargetLayer();

    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    const uint64 structureVersion =
        (store != nullptr && !mapGuid.IsNull()) ? store->StructureVersion(mapGuid) : 0;

    // Visibility lives on the entity and is edited from the inspector as well,
    // which bumps no version: this one component read is the price of the row's
    // checkbox staying truthful.
    bool visible = false;
    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    if (world != nullptr && entity.IsValid())
    {
        if (const Components::TileMapLayerRef* layerRef =
                world->GetComponent<Components::TileMapLayerRef>(entity))
            visible = layerRef->Visible;
    }

    // Which layer is targeted changes nothing about the rows themselves, so it
    // moves the bar instead of rebuilding. Tearing the list down on every layer
    // switch reflowed the section and blinked the rows out for a frame.
    const bool structural = entity != m_ListedEntity || mapGuid != m_ListedMap ||
                            structureVersion != m_ListedStructureVersion ||
                            visible != m_ListedVisible;
    const bool targetMoved = target != m_ListedTarget;
    if (!structural && !targetMoved)
        return;

    // A target that arrived from outside (the selection sync) supersedes the
    // row the user last clicked: one bar, on the layer a stroke will hit, and
    // a batch selection from the old map does not survive the move.
    if (target != m_PendingActiveLayer)
    {
        m_PendingActiveLayer = target;
        m_Selected.assign(target != 0 ? 1 : 0, target);
    }

    m_ListedEntity = entity;
    m_ListedMap = mapGuid;
    m_ListedStructureVersion = structureVersion;
    m_ListedTarget = target;
    m_ListedVisible = visible;

    if (structural)
        RebuildRows();
    else
        SetActiveRow(target);
}

void TilePaletteLayerList::RebuildRows()
{
    if (m_Rows == nullptr)
        return;
    m_Rows->RemoveAllChildren();
    m_RowElements.clear();

    ECS::World* world = m_Context != nullptr ? m_Context->World : nullptr;
    TileMapECS::TileMapRuntimeStore* store = TileMapECS::TileMapRuntimeStore::TryGet();
    TileMap::TileMapData* data =
        (store != nullptr && !m_ListedMap.IsNull()) ? store->GetMap(m_ListedMap) : nullptr;
    if (data == nullptr)
    {
        m_Rows->AddChild(MakeLabel("tp-empty", "Select an entity with a Tile Map Layer component."));
        return;
    }

    const GUID mapGuid = m_ListedMap;

    // Rows follow draw order, not the asset's layer order: the renderer sorts on
    // each entity's ZOffset, so the list has to read the way the map looks or
    // "move up" would shuffle rows and change nothing on screen.
    struct RowEntry
    {
        uint32 LayerId = 0;
        const TileMap::TileMapLayerData* Layer = nullptr;
        ECS::EntityHandle Entity{};
        float32 ZOffset = 0.0f;
    };
    std::vector<RowEntry> rows;
    rows.reserve(data->Layers().size());
    for (const TileMap::TileMapLayerData& layer : data->Layers())
    {
        RowEntry entry;
        entry.LayerId = layer.Id;
        entry.Layer = &layer;
        if (world != nullptr)
            entry.Entity = Editor::TileLayerEdits::FindLayerEntity(*world, mapGuid, layer.Id);
        if (world != nullptr && entry.Entity.IsValid())
        {
            if (const Components::TileMapLayerRef* ref =
                    world->GetComponent<Components::TileMapLayerRef>(entry.Entity))
                entry.ZOffset = ref->ZOffset;
        }
        rows.push_back(entry);
    }
    // Frontmost first, the way every 2D tool lists layers. A larger ZOffset is
    // further from the camera, so ascending Z puts the nearest layer on top.
    std::stable_sort(rows.begin(), rows.end(),
                     [](const RowEntry& a, const RowEntry& b) { return a.ZOffset < b.ZOffset; });

    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        const RowEntry& entry = rows[rowIndex];
        const TileMap::TileMapLayerData& layer = *entry.Layer;
        const uint32 layerId = layer.Id;
        const ECS::EntityHandle rowEntity = entry.Entity;
        const Components::TileMapLayerRef* layerRef =
            (world != nullptr && rowEntity.IsValid())
                ? world->GetComponent<Components::TileMapLayerRef>(rowEntity)
                : nullptr;

        auto row = std::make_unique<UIElement>();
        row->AddClass("tp-layer-row");
        // The edit context is the authority on the target; m_PendingActiveLayer
        // covers the frame between a row click and the update that applies it.
        const bool contextTargets = m_EditContext != nullptr &&
                                    m_EditContext->TargetMap() == mapGuid &&
                                    m_EditContext->TargetLayer() == layerId;
        if (contextTargets || m_PendingActiveLayer == layerId)
            row->AddClass("active");

        // Visibility lives on the entity that draws the layer, so each row
        // toggles its own -- the whole map is reachable from one list instead of
        // only the layer whose entity is selected.
        auto visibility = std::make_unique<Checkbox>();
        visibility->SetId("TilePaletteLayerVisible" + std::to_string(layerId));
        visibility->AddClass("tp-layer-visible");
        visibility->SetValueWithoutNotify(layerRef != nullptr && layerRef->Visible);
        const bool togglable = layerRef != nullptr;
        visibility->SetEnabled(togglable);
        visibility->SetTooltip(togglable ? "Show this layer in the scene"
                                         : "No entity in this scene draws this layer");
        if (togglable)
        {
            visibility->SetOnValueChanged([this, rowEntity](const bool& shown) {
                ECS::World* w = m_Context != nullptr ? m_Context->World : nullptr;
                auto* s = TileMapECS::TileMapRuntimeStore::TryGet();
                if (w == nullptr || s == nullptr)
                    return;
                const LayerStateSnapshot before = CaptureState();
                if (Components::TileMapLayerRef* ref =
                        w->GetComponentForWrite<Components::TileMapLayerRef>(rowEntity))
                {
                    ref->Visible = shown;
                    if (rowEntity == m_ListedEntity)
                        m_ListedVisible = shown;
                }
                CommitLayerEdit("Toggle Layer Visibility", before);
            });
        }
        row->AddChild(std::move(visibility));

        // Lock guards the map, so unlike visibility it lives on the layer and
        // applies to every entity drawing it. Open padlock when unlocked,
        // closed when locked.
        auto locked = std::make_unique<Button>();
        locked->SetId("TilePaletteLayerLock" + std::to_string(layerId));
        locked->AddClass("tp-layer-lock");
        if (layer.Locked)
            locked->AddClass("locked");
        locked->SetTooltip(layer.Locked ? "Locked: this layer refuses paint"
                                        : "Lock this layer against painting");
        locked->RegisterEventHandler(kEventButtonClick, [this, mapGuid, layerId](UIEvent&) {
            const LayerStateSnapshot before = CaptureState();
            auto* s = TileMapECS::TileMapRuntimeStore::TryGet();
            TileMap::TileMapData* map = s != nullptr ? s->GetMap(mapGuid) : nullptr;
            TileMap::TileMapLayerData* target = map != nullptr ? map->FindLayer(layerId) : nullptr;
            if (target == nullptr)
                return;
            target->Locked = !target->Locked;
            s->MarkStructuralChange(mapGuid);
            CommitLayerEdit("Lock Layer", before);
        });
        row->AddChild(std::move(locked));

        // Up/Down swap the two entities' ZOffsets, because that -- not the
        // asset's layer order -- is what the renderer sorts on. Reordering the
        // vector instead would move the row and leave the picture unchanged.
        // Handles are captured by value: these lambdas outlive this rebuild, so
        // they must not reach back into the row vector that produced them.
        auto reorder = [this, mapGuid](ECS::EntityHandle a, ECS::EntityHandle b) {
            ECS::World* w = m_Context != nullptr ? m_Context->World : nullptr;
            auto* s = TileMapECS::TileMapRuntimeStore::TryGet();
            if (w == nullptr || s == nullptr || !a.IsValid() || !b.IsValid())
                return;
            const LayerStateSnapshot before = CaptureState();
            auto* refA = w->GetComponentForWrite<Components::TileMapLayerRef>(a);
            auto* refB = w->GetComponentForWrite<Components::TileMapLayerRef>(b);
            if (refA == nullptr || refB == nullptr)
                return;
            std::swap(refA->ZOffset, refB->ZOffset);
            s->MarkStructuralChange(mapGuid);
            CommitLayerEdit("Reorder Layer", before);
        };
        const bool canMoveUp = rowIndex > 0 && rowEntity.IsValid();
        const bool canMoveDown = rowIndex + 1 < rows.size() && rowEntity.IsValid();

        auto moveUp = MakeIconButton("TilePaletteLayerUp" + std::to_string(layerId),
                                     "Draw this layer in front of the one above", "arrow-up-icon");
        moveUp->AddClass("tp-layer-move");
        moveUp->SetEnabled(canMoveUp);
        if (canMoveUp)
        {
            const ECS::EntityHandle above = rows[rowIndex - 1].Entity;
            moveUp->RegisterEventHandler(kEventButtonClick, [reorder, rowEntity, above](UIEvent&)
                                         { reorder(rowEntity, above); });
        }
        row->AddChild(std::move(moveUp));

        auto moveDown = MakeIconButton("TilePaletteLayerDown" + std::to_string(layerId),
                                       "Draw this layer behind the one below", "arrow-down-icon");
        moveDown->AddClass("tp-layer-move");
        moveDown->SetEnabled(canMoveDown);
        if (canMoveDown)
        {
            const ECS::EntityHandle below = rows[rowIndex + 1].Entity;
            moveDown->RegisterEventHandler(kEventButtonClick, [reorder, rowEntity, below](UIEvent&)
                                           { reorder(rowEntity, below); });
        }
        row->AddChild(std::move(moveDown));

        auto duplicate = MakeIconButton("TilePaletteLayerDuplicate" + std::to_string(layerId),
                                        "Duplicate this layer and its cells", "copy-icon");
        duplicate->AddClass("tp-layer-move");
        duplicate->RegisterEventHandler(kEventButtonClick,
                                        [this, layerId](UIEvent&) { DuplicateLayer(layerId); });
        row->AddChild(std::move(duplicate));

        // The name is the row's control: clicking it targets the layer, and it
        // edits in place, so renaming happens on the row rather than in a field
        // parked at the bottom of the panel.
        auto name = std::make_unique<TextField>();
        name->SetId("TilePaletteLayerName" + std::to_string(layerId));
        name->AddClass("tp-layer-name");
        TextField* nameField = name.get();
        // Renaming is a click-and-retype gesture, so entering the field selects
        // the whole name. TextField's own selects-all-on-mouse-focus path leaves
        // the caret where the press landed, which for a left-aligned name means
        // typing splices into the middle of the old one.
        name->RegisterEventHandler(kEventFocusIn, [this, nameField, rowEntity, layerId](UIEvent& e) {
            nameField->SelectAll();
            TargetLayer(rowEntity, layerId, e.Mods);
        });
        name->SetValueWithoutNotify(layer.Name.empty() ? ("Layer " + std::to_string(layerId))
                                                       : layer.Name);
        name->SetTooltip("Paint into this layer; type to rename it");
        name->SetOnCommit([this, mapGuid, layerId, nameField]() {
            auto* s = TileMapECS::TileMapRuntimeStore::TryGet();
            TileMap::TileMapData* map = s != nullptr ? s->GetMap(mapGuid) : nullptr;
            TileMap::TileMapLayerData* target = map != nullptr ? map->FindLayer(layerId) : nullptr;
            if (target == nullptr)
                return;
            const std::string typed = nameField->GetValue();
            // An empty box is a slip, not a request for a nameless layer.
            if (typed.empty() || typed == target->Name)
                return;
            const LayerStateSnapshot before = CaptureState();
            target->Name = typed;
            s->MarkStructuralChange(mapGuid);
            CommitLayerEdit("Rename Layer", before);
        });
        row->AddChild(std::move(name));

        // A press targets the layer and begins a potential reorder; the drag
        // only starts once the pointer has moved far enough that it cannot be
        // read as a click, so selecting a layer stays a plain click.
        row->RegisterEventHandler(kEventMouseMove, [this, layerId](UIEvent& e) {
            if (!m_DragPending || m_DragLayer != layerId)
                return;
            if (std::abs(e.Y - m_DragStartY) < kDragThresholdPx &&
                std::abs(e.X - m_DragStartX) < kDragThresholdPx)
                return;
            m_DragPending = false;
            UIManager* ui = GetOwnerManager();
            UI::Interaction::DragDropManager* dd = ui ? ui->GetDragDropManager() : nullptr;
            if (dd == nullptr)
                return;
            Editor::TilePaletteLayerReorderPayload payload{};
            payload.layerId = layerId;
            UI::Interaction::DragPayload dragPayload =
                UI::Interaction::DragPayload::Create(std::move(payload));
            dragPayload.DisplayLabel = m_DragLabel;
            UI::Interaction::DragSessionContext ctx{};
            ctx.SourceWidgetId = GetInstanceId();
            dd->BeginDrag(std::move(dragPayload), ctx);
        });
        row->RegisterEventHandler(kEventMouseUp, [this](UIEvent&) { m_DragPending = false; });
        row->RegisterEventHandler(kEventMouseDown, [this, rowEntity, layerId](UIEvent& e) {
            m_DragPending = true;
            m_DragLayer = layerId;
            m_DragStartX = e.X;
            m_DragStartY = e.Y;
            TargetLayer(rowEntity, layerId, e.Mods);
        });

        if (m_PendingActiveLayer == layerId)
            m_DragLabel = layer.Name;

        // Pushes the overflow glyph to the row's right end while the name keeps
        // hugging its text -- a stretched name field would make a click on the
        // empty strip start a rename instead of selecting the layer.
        auto spacer = std::make_unique<UIElement>();
        spacer->AddClass("tp-layer-spacer");
        row->AddChild(std::move(spacer));

        // Same glyph the inspector section headers use, at the row's right end.
        // The menu opens at the button so it appears under the thing clicked.
        auto settings = MakeIconButton("TilePaletteLayerMenu" + std::to_string(layerId),
                                       "Layer actions", "tp-layer-options");
        settings->AddClass("tp-layer-move");
        {
            UIElement* anchor = settings.get();
            settings->RegisterEventHandler(kEventButtonClick, [this, layerId, anchor](UIEvent&) {
                const int x = static_cast<int>(anchor->GetLayoutX());
                const int y = static_cast<int>(anchor->GetLayoutY() + anchor->GetLayoutHeight());
                ShowRowMenu(layerId, x, y);
            });
        }
        row->AddChild(std::move(settings));

        m_RowElements.emplace_back(layerId, row.get());
        m_Rows->AddChild(std::move(row));
    }

    if (data->Layers().empty())
        m_Rows->AddChild(MakeLabel("tp-empty", "This tilemap has no layers yet."));

    std::erase_if(m_Selected, [data](uint32 layerId) { return data->FindLayer(layerId) == nullptr; });
    ApplySelectionClasses();
}

} // namespace GameEngine
