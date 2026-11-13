#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "UI/UIElement.h"
#include "UI/Interaction/DropTarget.h"

namespace GameEngine
{

class Label;
class SearchDialog;
struct ISearchProvider;
struct SearchResultItem;
using SearchItemId = uint64_t;

/// Shared base for the inspector reference fields (AssetField, EntityField), built from
/// UI/controls/ReferenceField/ReferenceField.uxml. The picker button opens a SearchDialog; a click on
/// the rest of the field reveals the object when the subclass can, else opens the dialog.
class ReferenceFieldBase : public UIElement, public UI::Interaction::IDropTarget
{
public:
    ReferenceFieldBase();
    ~ReferenceFieldBase() override;

    // -- IDropTarget: generic parts (payload-specific overrides stay pure virtual) --
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

    /// Whether the field offers its clear button. Turn it off for a set-only
    /// reference — one whose null value is a state the user cannot author, so a
    /// clear would be an edit that something else silently undoes.
    void SetClearable(bool clearable);

    /// The text the field shows while it holds no reference: "None" unless set, for a field
    /// whose empty value means something else, such as a default it falls back to.
    void SetEmptyText(std::string text);

protected:
    // The child of the field's layout (UI/controls/ReferenceField/ReferenceField.uxml)
    // with this class; null when the layout did not load.
    UIElement* LayoutPart(std::string_view className) const;

    // Open the shared search dialog. No-op when one is already open or the subclass
    // is not ready (CreateProvider() returned null).
    void OpenReferenceDialog();

    // Display states for subclass UpdateDisplay(). The tooltip goes on the name.
    void ShowEmpty();                                                 // empty text, clear hidden
    void ShowNamed(const std::string& name, const std::string& tooltip);   // the name, clear shown
    void ShowMissing(const std::string& text, const std::string& tooltip); // a dangling value, clear shown

    // -- Subclass hooks --
    virtual std::unique_ptr<ISearchProvider> CreateProvider() = 0;
    virtual void OnResultSelected(const SearchResultItem& item) = 0;
    virtual void PositionDialog(SearchDialog& dialog) = 0;
    // Result id of the field's current value, so the dialog opens highlighting
    // it. 0 when the field is empty.
    virtual SearchItemId CurrentResultId() const = 0;
    virtual void OnClearRequested() = 0;
    // Whether a click on the field reveals the referenced object instead of opening
    // the dialog. Call RefreshLinkStyle() when the answer can change.
    virtual bool CanReveal() const = 0;
    virtual void Reveal() = 0;
    void RefreshLinkStyle();

    Label* m_NameLabel = nullptr;
    UIElement* m_ClearBtn = nullptr;
    UIElement* m_PickBtn = nullptr;
    bool m_Clearable = true;
    std::string m_EmptyText = "None";

private:
    // Move the layout's parts into the field and wire the clear, picker and field clicks.
    void BuildFromLayout();

    std::unique_ptr<ISearchProvider> m_SearchProvider; // kept alive while the dialog is open
    SearchDialog* m_ActiveDialog = nullptr;
};

} // namespace GameEngine
