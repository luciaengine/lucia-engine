#include "Rendering/Sorting/SortingLayerSettingsView.h"

#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/SortingLayers.h"
#include "Logger/Logger.h"
#include "Rendering/Sorting/SortingLayerSettings.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace GameEngine::Editor
{
SortingLayerSettingsView::SortingLayerSettingsView()
{
    RequestSubtreeStyleAssetPath("UI/panels/SortingLayers/SortingLayers.css", "editor");
}

void SortingLayerSettingsView::OnPostLayout()
{
    if (m_Loaded && m_Revision != Rendering::SortingLayers::Revision())
    {
        m_Revision = Rendering::SortingLayers::Revision();
        PostAction([this]() { ReloadLayers(); });
    }
    if (!m_Loaded && !m_LoadQueued && GetOwnerManager())
    {
        m_LoadQueued = true;
        PostAction([this]() { LoadLayout(); });
    }
}

void SortingLayerSettingsView::LoadLayout()
{
    auto* manager = GetOwnerManager();
    if (!manager || !manager->InstantiateLayoutChildrenFromAssetPath(
            this, "editor:UI/panels/SortingLayers/SortingLayers.uxml"))
    {
        Logger::Log::Error("Sorting Layers: cannot load the settings layout.");
        return;
    }
    m_Loaded = true;
    m_Layers = FindById("SortingLayerList");
    m_Error = dynamic_cast<Label*>(FindById("SortingLayerError"));
    auto* add = dynamic_cast<Button*>(FindById("SortingLayerAdd"));
    if (!m_Layers || !m_Error || !add)
    {
        Logger::Log::Error("Sorting Layers: settings layout is missing its list, error label, or add button.");
        return;
    }
    add->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { AddLayer(); });
    ReloadLayers();
}

void SortingLayerSettingsView::ReloadLayers()
{
    if (!m_Layers)
        return;
    m_Layers->RemoveAllChildren();
    auto layers = Rendering::SortingLayers::Current();
    std::sort(layers.Layers.begin(), layers.Layers.end(),
        [](const auto& left, const auto& right) { return left.Order < right.Order; });
    for (const auto& layer : layers.Layers)
        AppendLayer(layer);
    m_Revision = Rendering::SortingLayers::Revision();
}

void SortingLayerSettingsView::AppendLayer(const Rendering::SortingLayer& layer)
{
    auto row = std::make_unique<UIElement>();
    auto* rowPointer = row.get();
    m_Layers->AddChild(std::move(row));
    if (!GetOwnerManager()->InstantiateLayoutChildrenFromAssetPath(
            rowPointer, "editor:UI/panels/SortingLayers/SortingLayerRow.uxml"))
    {
        Logger::Log::Error("Sorting Layers: cannot load the layer row layout.");
        return;
    }
    auto* name = dynamic_cast<TextField*>(rowPointer->FindById("LayerName"));
    auto* priority = dynamic_cast<TextField*>(rowPointer->FindById("LayerPriority"));
    auto* identity = dynamic_cast<Label*>(rowPointer->FindById("LayerIdentity"));
    auto* remove = dynamic_cast<Button*>(rowPointer->FindById("LayerRemove"));
    auto* back = dynamic_cast<Button*>(rowPointer->FindById("LayerBack"));
    auto* forward = dynamic_cast<Button*>(rowPointer->FindById("LayerForward"));
    if (!name || !priority || !identity || !remove || !back || !forward)
    {
        Logger::Log::Error("Sorting Layers: layer row layout is missing a required field.");
        return;
    }
    identity->SetText("Layer ID " + std::to_string(layer.Id));
    name->SetValueWithoutNotify(layer.Name);
    priority->SetValueWithoutNotify(std::to_string(layer.Order));
    name->SetOnValueChanged([this, id = layer.Id, name](const std::string&) { CommitField(id, *name, false); });
    priority->SetOnValueChanged([this, id = layer.Id, priority](const std::string&) { CommitField(id, *priority, true); });
    const auto layers = Rendering::SortingLayers::Current();
    back->SetEnabled(std::any_of(layers.Layers.begin(), layers.Layers.end(),
        [&layer](const auto& candidate) { return candidate.Order < layer.Order; }));
    forward->SetEnabled(std::any_of(layers.Layers.begin(), layers.Layers.end(),
        [&layer](const auto& candidate) { return candidate.Order > layer.Order; }));
    back->RegisterEventHandler(kEventButtonClick,
        [this, id = layer.Id](UIEvent&) { MoveLayer(id, true); });
    forward->RegisterEventHandler(kEventButtonClick,
        [this, id = layer.Id](UIEvent&) { MoveLayer(id, false); });
    UI::Layout::SetElementHidden(*remove, layer.Id == 0);
    remove->RegisterEventHandler(kEventButtonClick,
        [this, id = layer.Id](UIEvent&) { RemoveLayer(id); });
}

bool SortingLayerSettingsView::Save(const Rendering::SortingLayers& layers)
{
    std::string error;
    const bool saved = SaveSortingLayerSettings(EngineCore::GetInstance().GetWorkspaceRoot(), layers, error);
    m_Error->SetText(error);
    UI::Layout::SetElementHidden(*m_Error, saved);
    if (saved)
        m_Revision = Rendering::SortingLayers::Revision();
    return saved;
}

void SortingLayerSettingsView::CommitField(int32 id, TextField& field, bool priority)
{
    auto layers = Rendering::SortingLayers::Current();
    for (auto& layer : layers.Layers)
    {
        if (layer.Id != id)
            continue;
        const std::string previous = priority ? std::to_string(layer.Order) : layer.Name;
        if (field.GetValue() == previous)
            return;
        if (priority)
        {
            const auto& text = field.GetValue();
            int32 value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            {
                m_Error->SetText("Priority must be a signed whole number.");
                UI::Layout::SetElementHidden(*m_Error, false);
                field.SetValueWithoutNotify(previous);
                return;
            }
            layer.Order = value;
        }
        else
            layer.Name = field.GetValue();
        if (!Save(layers))
            field.SetValueWithoutNotify(previous);
        else if (priority)
            PostAction([this]() { ReloadLayers(); });
        return;
    }
}

void SortingLayerSettingsView::AddLayer()
{
    auto layers = Rendering::SortingLayers::Current();
    auto store = OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
    std::string error;
    if (!store.Load(&error))
    {
        m_Error->SetText(error);
        UI::Layout::SetElementHidden(*m_Error, false);
        return;
    }
    int64_t nextId = 1;
    store.TryGetInt64("sortingLayerNextId", nextId);
    for (const auto& layer : layers.Layers)
        nextId = std::max(nextId, static_cast<int64_t>(layer.Id) + 1);
    if (nextId > std::numeric_limits<int32>::max())
    {
        m_Error->SetText("No sorting layer IDs remain in this project.");
        UI::Layout::SetElementHidden(*m_Error, false);
        return;
    }
    int32 id = static_cast<int32>(nextId);
    while (std::any_of(layers.Layers.begin(), layers.Layers.end(),
                      [id](const auto& layer) { return layer.Id == id || layer.Order == id; }))
    {
        if (id == std::numeric_limits<int32>::max())
            return;
        ++id;
    }
    std::string name = "Layer " + std::to_string(id);
    while (std::any_of(layers.Layers.begin(), layers.Layers.end(),
                      [&name](const auto& layer) { return layer.Name == name; }))
        name += " new";
    layers.Layers.push_back({id, name, id});
    if (Save(layers))
        PostAction([this]() { ReloadLayers(); });
}

void SortingLayerSettingsView::MoveLayer(int32 id, bool towardsBack)
{
    auto layers = Rendering::SortingLayers::Current();
    std::sort(layers.Layers.begin(), layers.Layers.end(),
        [](const auto& left, const auto& right) { return left.Order < right.Order; });
    for (std::size_t index = 0; index < layers.Layers.size(); ++index)
    {
        if (layers.Layers[index].Id != id)
            continue;
        if ((towardsBack && index == 0) || (!towardsBack && index + 1 == layers.Layers.size()))
            return;
        const std::size_t other = towardsBack ? index - 1 : index + 1;
        std::swap(layers.Layers[index].Order, layers.Layers[other].Order);
        if (Save(layers))
            PostAction([this]() { ReloadLayers(); });
        return;
    }
}

void SortingLayerSettingsView::RemoveLayer(int32 id)
{
    if (id == 0)
        return;
    auto layers = Rendering::SortingLayers::Current();
    std::erase_if(layers.Layers, [id](const auto& layer) { return layer.Id == id; });
    if (Save(layers))
        PostAction([this]() { ReloadLayers(); });
}
}
