#include "Editor/Hierarchy/LayerControls/HierarchyLayerPicker.h"

#include "Engine/Rendering/SortingLayers.h"
#include "Rendering/RenderLayerMask.h"
#include "Rendering/RenderLayersScriptApi.h"

#include <algorithm>
#include <cstdio>

namespace GameEngine::Editor
{

void HierarchyLayerPicker::PresentSorting(int32 current)
{
    const auto revision = Rendering::SortingLayers::Revision();
    if (!m_Revision || *m_Revision != revision)
    {
        auto layers = Rendering::SortingLayers::Current();
        std::sort(layers.Layers.begin(), layers.Layers.end(),
                  [](const auto& a, const auto& b) { return a.Order < b.Order; });
        m_Base.clear();
        for (const auto& layer : layers.Layers)
            m_Base.push_back({std::to_string(layer.Id), layer.Name});
        m_Revision = revision;
        m_Installed = false;
    }
    const std::string value = std::to_string(current);
    const auto found = std::find_if(m_Base.begin(), m_Base.end(),
                                    [&value](const Option& option) { return option.value == value; });
    const std::optional<int32> missing = found == m_Base.end() ? std::optional<int32>(current) : std::nullopt;
    const int selected = static_cast<int>(found - m_Base.begin());
    if (!m_Installed || missing != m_Missing)
    {
        auto options = m_Base;
        if (missing)
            options.push_back({value, "Missing layer (" + value + ")"});
        SetOptions(options, selected);
        m_Missing = missing;
        m_Installed = true;
    }
    else if (GetSelectedIndex() != selected)
    {
        SetSelectedIndexWithoutNotify(selected);
    }
}

void HierarchyLayerPicker::PresentVisibility(uint32 mask, uint64 namesRevision)
{
    const bool oneLayer = mask != 0 && (mask & (mask - 1u)) == 0;
    const std::optional<uint32> extra = oneLayer ? std::nullopt : std::optional<uint32>(mask);
    if (!m_Installed || extra != m_ExtraMask || !m_Revision || *m_Revision != namesRevision)
    {
        std::vector<Option> options;
        if (extra)
        {
            char label[32]{};
            std::snprintf(label, sizeof(label), "Mask 0x%08X", mask);
            options.push_back({std::to_string(mask), label});
        }
        for (uint32 layer = 0; layer < Rendering::RenderLayerMask::kCount; ++layer)
        {
            const Rendering::RenderLayerMask single = Rendering::RenderLayerMask::FromLayer(layer);
            options.push_back({std::to_string(single.Bits), Rendering::Script::NameOf(single)});
        }
        SetOptions(options, 0);
        m_ExtraMask = extra;
        m_Revision = namesRevision;
        m_Installed = true;
    }
    int selected = 0;
    if (oneLayer)
        for (uint32 layer = 0; layer < Rendering::RenderLayerMask::kCount; ++layer)
            if ((uint32{1} << layer) == mask)
                selected = static_cast<int>(layer);
    if (GetSelectedIndex() != selected)
        SetSelectedIndexWithoutNotify(selected);
}

} // namespace GameEngine::Editor
