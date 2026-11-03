#pragma once

#include "AgentCallRows.h"
#include "AssistantActionLedger.h"

#include "UI/UIElement.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
class AgentCallImageView;
class Button;
class Label;

/// One call row under a reply in the AI Assistant panel (AgentCallRowModel): the glyph,
/// the action, its summary and what it left in the undo history on one line; Allow,
/// Allow for this turn and Deny under a call that waits for the user's answer; the image
/// its result names inline and the assets it names as tiles with Open; the raw call and the
/// editor's response behind Details.
class AgentCallRow final : public UIElement
{
public:
    /// Called with the waiting call's request id and the user's answer.
    using AnswerHandler = std::function<void(uint64_t requestId, AssistantAnswer answer)>;

    /// `idSuffix` makes the row's element ids unique ("<message>:<call>").
    AgentCallRow(std::string idSuffix, AnswerHandler onAnswer);

    /// Shows `model`, touching only what changed since the last call; an open Details
    /// stays open.
    void Show(const AgentCallRowModel& model);
    /// The Allow button while the call waits for the user's answer; null otherwise.
    UIElement* AnswerFocus() const;
    /// Stacks the row for a narrow panel: the summary under the action, the undo state and
    /// Details beside it when they fit and on the next line when they do not, without the
    /// wide layout's width cap.
    void SetStacked(bool stacked);
    /// Sizes the row's inline image to the row's width (AgentCallImageView::Fit). Returns true
    /// while its size still moved.
    bool FitImage();

private:
    void Answer(AssistantAnswer answer);
    void ToggleDetails();
    void OnDetailsMouseUp(UIEvent& event);
    void OnDetailsKeyDown(UIEvent& event);
    void ShowDetails(const std::vector<AgentCallPieces>& lines);
    void ShowMedia(const AgentCallRowModel& model);
    void ShowResources(const std::vector<std::string>& references);

    std::string m_IdSuffix;
    AnswerHandler m_OnAnswer;
    UIElement* m_Line = nullptr;
    UIElement* m_Glyph = nullptr;
    Label* m_Action = nullptr;
    UIElement* m_Summary = nullptr;
    /// Ends the glyph and action's line in a stacked row; hidden otherwise.
    UIElement* m_Break = nullptr;
    /// The undo state and Details: one unit at the line's end that wraps as a whole.
    UIElement* m_Trail = nullptr;
    Label* m_UndoState = nullptr;
    Label* m_DetailsToggle = nullptr;
    UIElement* m_AnswerLine = nullptr;
    Button* m_Allow = nullptr;
    Button* m_AllowForTurn = nullptr;
    Button* m_Deny = nullptr;
    Label* m_Waiting = nullptr;
    /// The inline image and the asset tiles, between the answer line and Details.
    UIElement* m_Media = nullptr;
    AgentCallImageView* m_Image = nullptr;
    UIElement* m_Resources = nullptr;
    UIElement* m_Details = nullptr;
    bool m_DetailsOpen = false;
    /// The Details lines changed while closed; they are built when Details opens.
    bool m_DetailsStale = true;
    /// What the elements show now; nullopt before the first Show().
    std::optional<AgentCallRowModel> m_Shown;
};
} // namespace GameEngine
