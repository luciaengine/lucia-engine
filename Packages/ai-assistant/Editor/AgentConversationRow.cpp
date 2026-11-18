#include "AgentConversationRow.h"

#include "AgentReplyActions.h"

#include "Logger/Logger.h"
#include "Platform/Clipboard.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/MarkdownText.h"
#include "UI/Controls/TextArea.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{
constexpr const char* kLayoutAssetPath = "ai-assistant:Editor/UI/controls/AgentConversationRow.uxml";

void SetClassIf(UIElement& element, const char* className, bool on)
{
    if (on)
        element.AddClass(className);
    else
        element.RemoveClass(className);
}

// Finds a template element by its id and gives it the row's own id.
template <typename T>
T* TakeElement(UIElement& row, const char* id, size_t index)
{
    T* element = dynamic_cast<T*>(row.FindById(HashStringId(id)));
    if (element)
        element->SetId(std::string(id) + ":" + std::to_string(index));
    return element;
}

// A text menu opens at the pointer, where the selection was right-clicked.
Mathematics::Vector2 PointerAnchor(const UIElement& element)
{
    if (const UIManager* ui = element.GetOwnerManager(); ui && ui->IsMousePositionKnown())
        return ui->GetMousePosition();
    const Mathematics::Rect drawn = element.GetWindowRect();
    return {drawn.X, drawn.Bottom()};
}

// The right-click menu of a text part: Copy while text is selected, no menu without a selection.
std::vector<ContextMenuManipulator::Item> CopyItems(std::string selected)
{
    if (selected.empty())
        return {};
    ContextMenuManipulator::Item copy;
    copy.Path = "Copy";
    copy.OnActivate = [text = std::move(selected)] { Platform::SetClipboardText(text.c_str()); };
    return {std::move(copy)};
}

template <typename TextControl>
void AddCopyMenu(TextControl& control)
{
    auto menu = ContextMenuManipulator::Create([&control] { return CopyItems(control.GetSelectedText()); });
    menu->SetAnchor(&PointerAnchor);
    control.AddManipulator(menu);
}
} // namespace

bool PinHeightPx(UIElement& element, float heightPx)
{
    const auto existing = element.Style().Get(Style::Height);
    if (existing.has_value() && existing->IsPx() && std::fabs(existing->Value - heightPx) <= 0.5f)
        return false;

    element.Style().Set(Style::Height, StyleLength::Px(heightPx));
    element.MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
    return true;
}

AgentConversationRow::AgentConversationRow(size_t index, std::function<void()> onStop, std::function<void()> onRetry,
                                           ReplyActionHandler onReplyAction, AgentCallRow::AnswerHandler onAnswer,
                                           std::function<void()> onUndoTurn, MarkupChipHandler onFrameMarkup)
    : m_Index(index)
    , m_OnStop(std::move(onStop))
    , m_OnRetry(std::move(onRetry))
    , m_OnReplyAction(std::move(onReplyAction))
    , m_OnAnswer(std::move(onAnswer))
    , m_OnUndoTurn(std::move(onUndoTurn))
    , m_OnFrameMarkup(std::move(onFrameMarkup))
{
    SetFocusable(false);
}

bool AgentConversationRow::Build(UIManager& ui)
{
    if (!ui.InstantiateLayoutChildrenFromAssetPath(this, kLayoutAssetPath))
    {
        Logger::Log::Error("AI Assistant: {} did not instantiate; check that the package's assets are staged.",
                           kLayoutAssetPath);
        return false;
    }

    m_Header = TakeElement<Label>(*this, "AgentRowHeader", m_Index);
    m_Time = TakeElement<Label>(*this, "AgentRowTime", m_Index);
    m_Stop = TakeElement<Button>(*this, "AgentRowStop", m_Index);
    m_Retry = TakeElement<Button>(*this, "AgentRowRetry", m_Index);
    m_UndoTurn = TakeElement<Button>(*this, "AgentRowUndoTurn", m_Index);
    m_Retried = TakeElement<Label>(*this, "AgentRowRetried", m_Index);
    m_Text = TakeElement<TextArea>(*this, "AgentRowText", m_Index);
    m_Reply = TakeElement<MarkdownText>(*this, "AgentRowReply", m_Index);
    m_Calls = TakeElement<UIElement>(*this, "AgentRowCalls", m_Index);
    m_Status = TakeElement<UIElement>(*this, "AgentRowStatus", m_Index);
    m_StatusText = TakeElement<TextArea>(*this, "AgentRowStatusText", m_Index);
    m_Actions = TakeElement<UIElement>(*this, "AgentRowActions", m_Index);
    m_FileChips = TakeElement<UIElement>(*this, "AgentRowFiles", m_Index);
    m_MarkupChips = TakeElement<UIElement>(*this, "AgentRowMarkups", m_Index);
    m_SetupRefusalHost = TakeElement<UIElement>(*this, "AgentRowSetupRefusal", m_Index);
    if (!m_Header || !m_Time || !m_Stop || !m_Retry || !m_UndoTurn || !m_Retried || !m_Text || !m_Reply || !m_Calls ||
        !m_Status || !m_StatusText || !m_Actions || !m_FileChips || !m_MarkupChips || !m_SetupRefusalHost)
    {
        Logger::Log::Error("AI Assistant: {} is missing a row element.", kLayoutAssetPath);
        return false;
    }
    AddCopyMenu(*m_Text);
    AddCopyMenu(*m_Reply);
    AddCopyMenu(*m_StatusText);
    // The settings page's Setup row shows the same finding in the same control.
    auto refusal = std::make_unique<EditorUI::InspectorNotice>("", EditorUI::InspectorNotice::Kind::Error);
    m_SetupRefusal = refusal.get();
    m_SetupRefusalHost->AddChild(std::move(refusal));

    m_Stop->SetTooltip("Stop this reply and every queued prompt (Esc)");
    m_Stop->SetOnClick([this](UIEvent&) { m_OnStop(); });
    m_Retry->SetTooltip("Send this prompt again as a new message");
    m_Retry->SetOnClick([this](UIEvent&) { m_OnRetry(); });
    m_UndoTurn->SetOnClick([this](UIEvent&) { m_OnUndoTurn(); });
    return true;
}

void AgentConversationRow::Show(const AgentConversationRowModel& model, const std::string& time)
{
    if (!m_Text)
        return;
    const AgentConversationRowModel* shown = m_Shown ? &*m_Shown : nullptr;
    if (!shown)
    {
        m_Time->SetText(time);
        SetClassIf(*this, "agent-row-user", model.FromUser);
    }
    if (!shown || shown->FileChips != model.FileChips)
        BuildFileChips(model.FileChips);
    if (!shown || shown->MarkupChips != model.MarkupChips)
        BuildMarkupChips(model.MarkupChips);
    if (!shown || shown->Header != model.Header)
        m_Header->SetText(model.Header);
    if (!shown || shown->Text != model.Text || shown->FromUser != model.FromUser)
    {
        if (model.FromUser)
            m_Text->SetValue(model.Text);
        else
            m_Reply->SetMarkdown(model.Text);
    }
    if (!shown || shown->ShowsText != model.ShowsText || shown->FromUser != model.FromUser)
    {
        SetClassIf(*m_Text, "hidden", !model.ShowsText || !model.FromUser);
        SetClassIf(*m_Reply, "hidden", !model.ShowsText || model.FromUser);
    }
    if (!shown || shown->Calls != model.Calls)
        ShowCalls(model.Calls);
    if (!shown || shown->Asks != model.Asks)
        SetClassIf(*this, "agent-row-asks", model.Asks);
    if (!shown || shown->ShowsUndoTurn != model.ShowsUndoTurn || shown->CanUndoTurn != model.CanUndoTurn ||
        shown->UndoTurnTooltip != model.UndoTurnTooltip)
    {
        SetClassIf(*m_UndoTurn, "hidden", !model.ShowsUndoTurn);
        m_UndoTurn->SetEnabled(model.CanUndoTurn);
        m_UndoTurn->SetTooltip(model.UndoTurnTooltip);
    }
    if (!shown || shown->Status != model.Status || shown->Failed != model.Failed || shown->Stopped != model.Stopped)
        ShowStatus(model);
    if (!shown || shown->SetupRefusal != model.SetupRefusal)
    {
        m_SetupRefusal->SetText(model.SetupRefusal);
        SetClassIf(*m_SetupRefusalHost, "hidden", model.SetupRefusal.empty());
    }
    if (!shown || shown->Failed != model.Failed || shown->Retried != model.Retried)
    {
        SetClassIf(*m_Retry, "hidden", !model.Failed || model.Retried);
        SetClassIf(*m_Retried, "hidden", !model.Retried);
    }
    if (!shown || shown->CanStop != model.CanStop)
        SetClassIf(*m_Stop, "hidden", !model.CanStop);
    if (model.ShowsReplyActions && !m_ReplyActionsBuilt)
        BuildReplyActions(model.ReplyActions);
    m_Shown = model;
}

void AgentConversationRow::ShowStatus(const AgentConversationRowModel& model)
{
    SetClassIf(*m_Status, "hidden", model.Status.empty());
    SetClassIf(*m_StatusText, "agent-row-status-failed", model.Failed);
    SetClassIf(*m_StatusText, "agent-row-status-stopped", model.Stopped);
    m_StatusText->SetValue(model.Status);
}

void AgentConversationRow::ShowCalls(const std::vector<AgentCallRowModel>& calls)
{
    while (m_CallRows.size() > calls.size())
    {
        m_Calls->RemoveChild(m_CallRows.back());
        m_CallRows.pop_back();
    }
    while (m_CallRows.size() < calls.size())
    {
        auto row = std::make_unique<AgentCallRow>(std::to_string(m_Index) + ":" + std::to_string(m_CallRows.size()),
                                                  m_OnAnswer);
        row->SetStacked(m_CallsStacked);
        m_CallRows.push_back(row.get());
        m_Calls->AddChild(std::move(row));
    }
    for (size_t call = 0; call < calls.size(); ++call)
        m_CallRows[call]->Show(calls[call]);
    SetClassIf(*m_Calls, "hidden", calls.empty());
}

void AgentConversationRow::BuildFileChips(const std::vector<std::string>& files)
{
    m_FileChips->RemoveAllChildren();
    SetClassIf(*m_FileChips, "hidden", files.empty());
    for (const std::string& name : files)
    {
        auto label = std::make_unique<Label>();
        label->AddClass("agent-row-file-chip");
        label->SetText(name);
        m_FileChips->AddChild(std::move(label));
    }
}

void AgentConversationRow::BuildMarkupChips(const std::vector<MarkupChip>& chips)
{
    m_MarkupChips->RemoveAllChildren();
    SetClassIf(*m_MarkupChips, "hidden", chips.empty());
    for (const MarkupChip& chip : chips)
    {
        auto button = std::make_unique<Button>();
        button->AddClass("small");
        button->AddClass("secondary");
        button->AddClass("agent-row-markup-chip");
        button->SetFocusable(false);
        button->SetText(chip.Label);
        button->SetTooltip("Frame " + chip.Label + " in the Scene View");
        // To the chip's right: chips start at the panel's left edge, so a centered tooltip would
        // leave the panel.
        button->SetTooltipPlacement(UIElement::TooltipPlacement::Right);
        button->SetOnClick([this, token = chip.Token](UIEvent&) { m_OnFrameMarkup(token); });
        m_MarkupChips->AddChild(std::move(button));
    }
}

void AgentConversationRow::BuildReplyActions(const std::vector<std::string>& actionIds)
{
    m_ReplyActionsBuilt = true;
    const std::span<const AgentReplyAction> actions = AgentReplyActions::All();
    for (size_t actionIndex = 0; actionIndex < actions.size(); ++actionIndex)
    {
        const AgentReplyAction& action = actions[actionIndex];
        if (std::find(actionIds.begin(), actionIds.end(), action.Id) == actionIds.end())
            continue;
        auto button = std::make_unique<Button>();
        button->SetId("AgentRowAction:" + action.Id + ":" + std::to_string(m_Index));
        button->AddClass("small");
        button->AddClass("secondary");
        button->SetFocusable(false);
        button->SetText(action.Label);
        button->SetTooltip(action.Tooltip);
        button->SetOnClick([this, actionIndex, raw = button.get()](UIEvent&) {
            const AgentReplyAction& clicked = AgentReplyActions::All()[actionIndex];
            if (!m_OnReplyAction(clicked) || !clicked.Once)
                return;
            raw->SetEnabled(false);
            raw->SetTooltip(clicked.DoneTooltip);
        });
        m_Actions->AddChild(std::move(button));
    }
}

void AgentConversationRow::SetCallsStacked(bool stacked)
{
    m_CallsStacked = stacked;
    if (m_Status && m_Status->GetParent())
        SetClassIf(*m_Status->GetParent(), "agent-row-footer-stacked", stacked);
    if (m_Actions)
        SetClassIf(*m_Actions, "agent-row-actions-stacked", stacked);
    for (AgentCallRow* call : m_CallRows)
        call->SetStacked(stacked);
}

UIElement* AgentConversationRow::AnswerFocus() const
{
    for (AgentCallRow* call : m_CallRows)
        if (UIElement* allow = call->AnswerFocus())
            return allow;
    return nullptr;
}

void AgentConversationRow::RefreshTextVisuals()
{
    if (m_Text)
        m_Text->MarkDirty(VisualDirty);
    if (m_StatusText)
        m_StatusText->MarkDirty(VisualDirty);
}

bool AgentConversationRow::FitHeight()
{
    if (!m_Text)
        return false;
    // A reply (MarkdownText) is sized by layout; only a prompt's text area is pinned.
    float textHeight = 0.0f;
    bool moved = false;
    if (!m_Text->HasClass("hidden"))
    {
        const float advance = m_Text->GetResolvedLineAdvancePx();
        if (advance <= 0.0f)
            return false;
        const auto& textBox = m_Text->GetResolvedStyle().Layout;
        textHeight = static_cast<float>(m_Text->GetVisualLineCount()) * advance + textBox.Padding.Top +
                     textBox.Padding.Bottom + textBox.BorderWidth.Top + textBox.BorderWidth.Bottom;
        moved = PinHeightPx(*m_Text, textHeight);
    }
    // A text area does not size to its text: the status is pinned to its wrapped lines.
    if (m_StatusText && !m_Status->HasClass("hidden"))
    {
        const float advance = m_StatusText->GetResolvedLineAdvancePx();
        if (advance > 0.0f)
            moved = PinHeightPx(*m_StatusText, static_cast<float>(m_StatusText->GetVisualLineCount()) * advance) || moved;
    }
    for (AgentCallRow* call : m_CallRows)
    {
        moved = call->FitImage() || moved;
        moved = call->FitDetailsToggle() || moved;
        moved = call->FitReason() || moved;
    }

    float extent = 0.0f;
    const float top = GetLayoutY();
    for (const auto& child : GetChildren())
    {
        if (!child || child->HasClass("hidden"))
            continue;
        const float childHeight = child.get() == m_Text ? textHeight : child->GetLayoutHeight();
        extent = std::max(extent, child->GetLayoutY() - top + childHeight + child->GetResolvedStyle().Layout.Margin.Bottom);
    }
    const auto& rowBox = GetResolvedStyle().Layout;
    extent += rowBox.Padding.Bottom + rowBox.BorderWidth.Bottom;
    if (extent > 0.0f && PinHeightPx(*this, extent))
        moved = true;
    return moved;
}
} // namespace GameEngine
