#include "AgentCallRow.h"

#include "AgentCallImageView.h"
#include "AgentCallPieceView.h"
#include "AgentCallResourceResolver.h"
#include "AgentCallResourceTile.h"

#include "UI/Controls/Button.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Label.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIEvents.h"

#include <memory>
#include <optional>
#include <utility>

namespace GameEngine
{
namespace
{
using Mark = AgentCallRowModel::Mark;

// UIEvent::Button for the left mouse button.
constexpr int kLeftButton = 0;

// The glyph classes (PromptPanel.css), one per mark; None draws no glyph.
constexpr const char* kGlyphClasses[] = {"agent-call-glyph-none", "agent-call-glyph-done", "agent-call-glyph-view",
                                         "agent-call-glyph-refused", "agent-call-glyph-waiting"};

const char* GlyphClass(Mark mark)
{
    return kGlyphClasses[static_cast<size_t>(mark)];
}

void SetClassIf(UIElement& element, const char* className, bool on)
{
    if (on)
        element.AddClass(className);
    else
        element.RemoveClass(className);
}

// Adds a new `T` with `className` to `parent` and returns it.
template <typename T>
T* AddElement(UIElement& parent, const char* className)
{
    auto element = std::make_unique<T>();
    element->AddClass(className);
    T* added = element.get();
    parent.AddChild(std::move(element));
    return added;
}

Button* AddButton(UIElement& parent, const std::string& id, const char* text, const char* variant,
                  const char* tooltip)
{
    Button* button = AddElement<Button>(parent, "small");
    button->AddClass(variant);
    button->SetId(id);
    button->SetText(text);
    button->SetTooltip(tooltip);
    return button;
}
} // namespace

AgentCallRow::AgentCallRow(std::string idSuffix, AnswerHandler onAnswer)
    : m_IdSuffix(idSuffix)
    , m_OnAnswer(std::move(onAnswer))
{
    SetFocusable(false);
    AddClass("agent-call");

    m_Line = AddElement<UIElement>(*this, "agent-call-line");
    m_Glyph = AddElement<UIElement>(*m_Line, "agent-call-glyph");
    m_Action = AddElement<Label>(*m_Line, "agent-call-action");
    m_Break = AddElement<UIElement>(*m_Line, "agent-call-break");
    m_Break->AddClass("hidden");
    m_Summary = AddElement<UIElement>(*m_Line, "agent-call-summary");
    m_Trail = AddElement<UIElement>(*m_Line, "agent-call-trail");
    m_UndoState = AddElement<Label>(*m_Trail, "agent-call-undo");
    // A text link, not a button: it takes no plate, and Enter or Space toggles it.
    m_DetailsToggle = AddElement<Label>(*m_Trail, "agent-call-details-toggle");
    m_DetailsToggle->SetId("AgentCallDetails:" + idSuffix);
    m_DetailsToggle->SetText("Details");
    m_DetailsToggle->SetTooltip("The call the assistant sent and the editor's answer");
    m_DetailsToggle->SetFocusable(true);
    m_DetailsToggle->RegisterEventHandler(kEventMouseUp, [this](UIEvent& event) { OnDetailsMouseUp(event); });
    m_DetailsToggle->RegisterEventHandler(kEventKeyDown, [this](UIEvent& event) { OnDetailsKeyDown(event); });

    m_AnswerLine = AddElement<UIElement>(*this, "agent-call-answer");
    m_Allow = AddButton(*m_AnswerLine, "AgentCallAllow:" + idSuffix, "Allow", "primary", "Run this call once");
    m_AllowForTurn = AddButton(*m_AnswerLine, "AgentCallAllowTurn:" + idSuffix, "Allow for this turn", "secondary",
                               "Run this call and every later call of the same tool in this reply");
    m_Deny = AddButton(*m_AnswerLine, "AgentCallDeny:" + idSuffix, "Deny", "secondary",
                       "Do not run this call; the assistant is told you declined");
    m_Allow->SetOnClick([this](UIEvent&) { Answer(AssistantAnswer::Allow); });
    m_AllowForTurn->SetOnClick([this](UIEvent&) { Answer(AssistantAnswer::AllowForTurn); });
    m_Deny->SetOnClick([this](UIEvent&) { Answer(AssistantAnswer::Deny); });
    m_Waiting = AddElement<Label>(*m_AnswerLine, "agent-call-waiting");

    m_Media = AddElement<UIElement>(*this, "agent-call-media");
    m_Media->AddClass("hidden");
    auto image = std::make_unique<AgentCallImageView>(idSuffix);
    image->AddClass("hidden");
    m_Image = image.get();
    m_Media->AddChild(std::move(image));
    m_Resources = AddElement<UIElement>(*m_Media, "agent-call-resources");
    m_Resources->AddClass("hidden");

    m_Details = AddElement<UIElement>(*this, "agent-call-details");
    m_Details->AddClass("hidden");
}

void AgentCallRow::Show(const AgentCallRowModel& model)
{
    const AgentCallRowModel* shown = m_Shown ? &*m_Shown : nullptr;
    if (!shown || shown->Glyph != model.Glyph)
    {
        if (shown)
            m_Glyph->RemoveClass(GlyphClass(shown->Glyph));
        m_Glyph->AddClass(GlyphClass(model.Glyph));
    }
    if (!shown || shown->Action != model.Action)
    {
        m_Action->SetText(model.Action);
        m_AllowForTurn->SetTooltip("Run this call and every later '" + model.Action + "' call in this reply");
    }
    if (!shown || shown->Summary != model.Summary)
        ShowAgentCallPieces(*m_Summary, model.Summary, AgentCallPieceSurface::Summary);
    if (!shown || shown->UndoState != model.UndoState || shown->Glyph != model.Glyph)
    {
        m_UndoState->SetText(model.UndoState);
        SetClassIf(*m_UndoState, "hidden", model.UndoState.empty());
        SetClassIf(*m_UndoState, "agent-call-undo-refused", model.Glyph == Mark::Refused);
    }
    if (!shown || shown->Asks != model.Asks || shown->Waiting != model.Waiting)
    {
        SetClassIf(*m_AnswerLine, "hidden", model.Waiting.empty());
        for (Button* button : {m_Allow, m_AllowForTurn, m_Deny})
            SetClassIf(*button, "hidden", !model.Asks);
        m_Waiting->SetText(model.Waiting);
    }
    if (!shown || shown->Image != model.Image || shown->Resources != model.Resources)
        ShowMedia(model);
    m_DetailsStale = m_DetailsStale || !shown || shown->Details != model.Details;
    m_Shown = model;
    if (m_DetailsOpen && m_DetailsStale)
        ShowDetails(m_Shown->Details);
}

void AgentCallRow::ShowMedia(const AgentCallRowModel& model)
{
    const AgentCallRowModel* shown = m_Shown ? &*m_Shown : nullptr;
    if (model.Image)
        m_Image->Show(*model.Image);
    SetClassIf(*m_Image, "hidden", !model.Image);
    if (!shown || shown->Resources != model.Resources)
        ShowResources(model.Resources);
    SetClassIf(*m_Media, "hidden", !model.Image && m_Resources->GetChildren().empty());
}

void AgentCallRow::ShowResources(const std::vector<std::string>& references)
{
    m_Resources->RemoveAllChildren();
    const std::optional<AgentCallResourceResolver> resolver = AgentCallResourceResolver::ForEngine();
    const std::vector<AgentCallResource> resources =
        resolver ? resolver->ResolveAll(references) : std::vector<AgentCallResource>();
    for (size_t index = 0; index < resources.size(); ++index)
    {
        auto tile = std::make_unique<AgentCallResourceTile>(resources[index], m_IdSuffix + ":" + std::to_string(index));
        AgentCallResourceTile* added = tile.get();
        m_Resources->AddChild(std::move(tile));
        added->RequestThumbnail();
    }
    SetClassIf(*m_Resources, "hidden", m_Resources->GetChildren().empty());
}

bool AgentCallRow::FitImage()
{
    if (m_Image->HasClass("hidden"))
        return false;
    const auto& box = m_Media->GetResolvedStyle().Layout;
    // The height cap is the image's CSS max-height.
    const StyleLength cap = m_Image->GetResolvedStyle().Layout.MaxHeight;
    if (!cap.IsPx())
        return false;
    return m_Image->Fit(m_Media->GetLayoutWidth() - box.Padding.Left - box.Padding.Right, cap.Value);
}

void AgentCallRow::SetStacked(bool stacked)
{
    SetClassIf(*m_Line, "agent-call-line-stacked", stacked);
    SetClassIf(*m_Break, "hidden", !stacked);
    SetClassIf(*m_Summary, "agent-call-summary-stacked", stacked);
    SetClassIf(*m_Trail, "agent-call-trail-stacked", stacked);
}

void AgentCallRow::OnDetailsMouseUp(UIEvent& event)
{
    if (event.Button != kLeftButton)
        return;
    event.Stop();
    ToggleDetails();
}

void AgentCallRow::OnDetailsKeyDown(UIEvent& event)
{
    if (event.Key != Input::kKeyCode_Enter && event.Key != Input::kKeyCode_Space)
        return;
    event.Stop();
    ToggleDetails();
}

UIElement* AgentCallRow::AnswerFocus() const
{
    return m_Shown && m_Shown->Asks ? m_Allow : nullptr;
}

void AgentCallRow::Answer(AssistantAnswer answer)
{
    if (m_Shown && m_Shown->Asks)
        m_OnAnswer(m_Shown->RequestId, answer);
}

void AgentCallRow::ShowDetails(const std::vector<AgentCallPieces>& lines)
{
    m_DetailsStale = false;
    m_Details->RemoveAllChildren();
    for (const AgentCallPieces& pieces : lines)
    {
        auto line = std::make_unique<UIElement>();
        line->AddClass("agent-call-details-line");
        ShowAgentCallPieces(*line, pieces, AgentCallPieceSurface::Details);
        m_Details->AddChild(std::move(line));
    }
}

void AgentCallRow::ToggleDetails()
{
    m_DetailsOpen = !m_DetailsOpen;
    if (m_DetailsOpen && m_DetailsStale && m_Shown)
        ShowDetails(m_Shown->Details);
    SetClassIf(*m_Details, "hidden", !m_DetailsOpen);
    m_DetailsToggle->SetText(m_DetailsOpen ? "Hide details" : "Details");
}
} // namespace GameEngine
