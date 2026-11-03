#pragma once

#include "UI/UIElement.h"
#include "Types/Types.h"

namespace GameEngine { class Label; class TextField; }
namespace GameEngine::Rendering { struct SortingLayer; struct SortingLayers; }
namespace GameEngine::Editor
{
/// Project sorting layers editor; committed fields show persisted values or an actionable error.
class SortingLayerSettingsView final : public UIElement
{
public:
    SortingLayerSettingsView();
    void OnPostLayout() override;
private:
    void LoadLayout();
    void ReloadLayers();
    void AppendLayer(const Rendering::SortingLayer& layer);
    void AddLayer();
    void MoveLayer(int32 id, bool towardsBack);
    void RemoveLayer(int32 id);
    void CommitField(int32 id, TextField& field, bool priority);
    bool Save(const Rendering::SortingLayers& layers);
    bool m_LoadQueued = false;
    bool m_Loaded = false;
    uint64 m_Revision = 0;
    UIElement* m_Layers = nullptr;
    Label* m_Error = nullptr;
};
}
