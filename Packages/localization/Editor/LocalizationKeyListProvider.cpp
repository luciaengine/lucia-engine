#include "LocalizationKeyListProvider.h"
#include <algorithm>
namespace GameEngine::Editor
{
void LocalizationKeyListProvider::SetRows(std::vector<Row> rows)
{
    m_Rows = std::move(rows);
    MarkAllChanged();
}
int LocalizationKeyListProvider::IndexOf(StringId id) const
{
    const auto found = std::find_if(m_Rows.begin(), m_Rows.end(), [id](const Row& row) { return row.Id == id; });
    return found == m_Rows.end() ? -1 : static_cast<int>(found - m_Rows.begin());
}
void LocalizationKeyListProvider::SetDirtyId(StringId id)
{
    if (id == m_DirtyId) return;
    const StringId previous = m_DirtyId;
    m_DirtyId = id;
    MarkRow(previous);
    MarkRow(id);
}
void LocalizationKeyListProvider::MarkRow(StringId id)
{
    const int index = id ? IndexOf(id) : -1;
    if (index >= 0) MarkChanged(GetItemId(index));
}
ListId LocalizationKeyListProvider::GetItemId(int index) const
{
    return index >= 0 && index < GetItemCount() ? static_cast<ListId>(index + 1) : 0;
}
}
