#include "UI/ReferenceFieldBase.h"

#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/ReferenceField/ReferenceField.uxml";
} // namespace

ReferenceFieldBase::ReferenceFieldBase()
{
    // Both reference fields share the asset-field stylesheet (bordered name +
    // clear button + drop-valid/invalid states).
    AddClass("asset-field");
    RequestSubtreeStyleAssetPath("UI/controls/AssetField.css", "editor");
    BuildFromLayout();
}

ReferenceFieldBase::~ReferenceFieldBase()
{
    // Tear down a still-open dialog. By now m_OwnerManager has been cleared
    // (RemoveChild -> SetOwnerManager(nullptr)), so reach the root via the
    // dialog's parent -- it was added directly to the root element.
    if (m_ActiveDialog)
    {
        if (UIElement* parent = m_ActiveDialog->GetParent())
            parent->RemoveChild(m_ActiveDialog);
        m_ActiveDialog = nullptr;
    }
    m_SearchProvider.reset();
}

UIElement* ReferenceFieldBase::LayoutPart(std::string_view className) const
{
    for (const auto& child : GetChildren())
    {
        if (child && child->HasClass(std::string(className)))
            return child.get();
    }
    return nullptr;
}

void ReferenceFieldBase::BuildFromLayout()
{
    std::unique_ptr<UIElement> layout = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, {});
    if (!layout)
        return;
    while (!layout->GetChildren().empty())
        AddChild(layout->TakeChild(layout->GetChildren().front().get()));

    auto* name = dynamic_cast<Label*>(LayoutPart("asset-field-name"));
    UIElement* clear = LayoutPart("asset-field-clear");
    UIElement* pick = LayoutPart("asset-field-pick");
    if (!name || !clear || !pick)
    {
        Logger::Log::Error("ReferenceField: {} needs the name label, the clear button and the picker button.",
                           kLayoutAssetPath);
        return;
    }
    m_NameLabel = name;
    m_ClearBtn = clear;
    m_PickBtn = pick;
    m_NameLabel->SetText(m_EmptyText);
    m_ClearBtn->SetTooltip("Clear");
    m_PickBtn->SetTooltip("Choose from a list");

    // Clear on mouse-up: a clear on mouse-down hides the button, and the release then
    // lands on the field. The field's mouse-up handler ignores a release on this button.
    m_ClearBtn->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (e.Button == 0)
        {
            OnClearRequested();
            e.Stop();
        }
    });

    // The field takes its own presses, so a host that claims presses (a graph
    // node's pin) leaves the release to the part under the pointer.
    RegisterEventHandler(kEventMouseDown, [](UIEvent& e) {
        if (e.Button == 0)
            e.Stop();
    });

    // Act on mouse-up so the release that opens the picker does not clear focus
    // from the dialog's search field the moment it appears.
    RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (e.Button != 0 || e.Target == m_ClearBtn)
            return;
        if (e.Target != m_PickBtn && CanReveal())
            Reveal();
        else
            OpenReferenceDialog();
        e.Stop();
    });
}

void ReferenceFieldBase::OpenReferenceDialog()
{
    if (m_ActiveDialog)
        return;

    UIManager* mgr = GetOwnerManager();
    if (!mgr)
        return;
    UIElement* root = mgr->GetRootElement();
    if (!root)
        return;

    std::unique_ptr<ISearchProvider> provider = CreateProvider();
    if (!provider)
        return;
    m_SearchProvider = std::move(provider);

    auto dialog = std::make_unique<SearchDialog>();
    dialog->SetProvider(m_SearchProvider.get());
    dialog->SetInitialSelection(CurrentResultId());

    SearchDialog* dialogPtr = dialog.get();
    m_ActiveDialog = dialogPtr;
    AddClass("open");

    dialog->SetOnResult([this, dialogPtr, root](const SearchResultItem& item) {
        OnResultSelected(item);
        m_ActiveDialog = nullptr;
        RemoveClass("open");
        m_SearchProvider.reset();
        root->RemoveChild(dialogPtr);
    });
    dialog->SetOnCancel([this, dialogPtr, root]() {
        m_ActiveDialog = nullptr;
        RemoveClass("open");
        m_SearchProvider.reset();
        root->RemoveChild(dialogPtr);
    });

    root->AddChild(std::move(dialog));
    PositionDialog(*dialogPtr);
    dialogPtr->Show();
}

void ReferenceFieldBase::ShowEmpty()
{
    if (!m_NameLabel)
        return;
    m_NameLabel->SetText(m_EmptyText);
    m_NameLabel->SetTooltip({});
    m_NameLabel->AddClass("empty");
    m_NameLabel->RemoveClass("missing");
    m_ClearBtn->AddClass("hidden");
    RefreshLinkStyle();
}

void ReferenceFieldBase::ShowNamed(const std::string& name, const std::string& tooltip)
{
    if (!m_NameLabel)
        return;
    m_NameLabel->SetText(name);
    m_NameLabel->SetTooltip(tooltip);
    m_NameLabel->RemoveClass("empty");
    m_NameLabel->RemoveClass("missing");
    if (m_Clearable)
        m_ClearBtn->RemoveClass("hidden");
    RefreshLinkStyle();
}

void ReferenceFieldBase::ShowMissing(const std::string& text, const std::string& tooltip)
{
    if (!m_NameLabel)
        return;
    m_NameLabel->SetText(text);
    m_NameLabel->SetTooltip(tooltip);
    m_NameLabel->RemoveClass("empty");
    m_NameLabel->AddClass("missing");
    if (m_Clearable)
        m_ClearBtn->RemoveClass("hidden");
    RefreshLinkStyle();
}

void ReferenceFieldBase::RefreshLinkStyle()
{
    if (!m_NameLabel)
        return;
    if (CanReveal())
        m_NameLabel->AddClass("link");
    else
        m_NameLabel->RemoveClass("link");
}

void ReferenceFieldBase::SetEmptyText(std::string text)
{
    m_EmptyText = std::move(text);
    if (m_NameLabel && m_NameLabel->HasClass("empty"))
        m_NameLabel->SetText(m_EmptyText);
}

void ReferenceFieldBase::SetClearable(bool clearable)
{
    m_Clearable = clearable;
    // .hidden is display:none, so the button leaves layout entirely and cannot be
    // hit-tested — the mouse-up handler it carries needs no further guard.
    if (!clearable && m_ClearBtn)
        m_ClearBtn->AddClass("hidden");
}

bool ReferenceFieldBase::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    if (!ContainsPoint(x, y))
        return false;
    out.TargetId = 0;
    out.Location = UI::Interaction::DropLocation::OnItem;
    return true;
}

void ReferenceFieldBase::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    if (!state.Visible)
    {
        RemoveClass("drop-valid");
        RemoveClass("drop-invalid");
        return;
    }
    if (state.Allowed)
    {
        AddClass("drop-valid");
        RemoveClass("drop-invalid");
    }
    else
    {
        RemoveClass("drop-valid");
        AddClass("drop-invalid");
    }
}

} // namespace GameEngine
