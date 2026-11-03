#pragma once
#include "UI/Controls/ListView.h"
#include "Types/StringId.h"
#include <string>
#include <vector>
namespace GameEngine::Editor
{
// The Localization panel's key list: one row per visible catalog key, in the order the
// panel filtered them. A row's ListId is its index + 1 (0 is "no row").
class LocalizationKeyListProvider final : public ListChangeTrackingProvider
{
public:
    struct Row
    {
        StringId Id;
        std::string Key;
        // No translation in the target language yet (LocalizationDocument::Missing).
        bool Missing = false;
        // The edited entry, listed although the filter would hide it.
        bool Pinned = false;
        bool Current = false;
    };
    void SetRows(std::vector<Row> rows);
    const std::vector<Row>& Rows() const { return m_Rows; }
    // The index of the row showing `id`, or -1.
    int IndexOf(StringId id) const;
    // The entry whose row shows the unapplied-changes mark; 0 for none. Rebinds the rows it moves between.
    void SetDirtyId(StringId id);
    StringId GetDirtyId() const { return m_DirtyId; }
    int GetItemCount() const override { return static_cast<int>(m_Rows.size()); }
    ListId GetItemId(int index) const override;
    float GetItemHeight(int) const override { return kRowHeight; }
    // Logical px; matches .localization-key-row in LocalizationPanel.css.
    static constexpr float kRowHeight = 26.0f;
private:
    void MarkRow(StringId id);
    std::vector<Row> m_Rows;
    StringId m_DirtyId = 0;
};
}
