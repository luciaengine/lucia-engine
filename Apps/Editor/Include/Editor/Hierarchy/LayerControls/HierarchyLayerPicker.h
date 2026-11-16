#pragma once

#include "Types/Types.h"
#include "UI/Controls/Dropdown.h"

#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

/// One layer dropdown on a pooled Hierarchy row: the Sorting Layer or the Render Layer of the row's entity.
/// The row keeps it when the tree binds the row to another entity, so a new entity only changes the
/// selection; the options are built again only when the layers or their names change. Main thread.
class HierarchyLayerPicker final : public Dropdown
{
public:
    /// Shows the project's sorting layers in their draw order with `current` selected. A layer id that is not
    /// in the project shows as "Missing layer (<id>)".
    void PresentSorting(int32 current);

    /// Shows one option per render layer, named from the project's names, with the layer of `mask` selected.
    /// A mask that is not exactly one layer shows first as "Mask 0x<hex>". `namesRevision` changes when the
    /// project renames a layer.
    void PresentVisibility(uint32 mask, uint64 namesRevision);

private:
    std::optional<uint64> m_Revision;
    std::optional<int32> m_Missing;
    std::optional<uint32> m_ExtraMask;
    std::vector<Option> m_Base;
    bool m_Installed = false;
};

} // namespace GameEngine::Editor
