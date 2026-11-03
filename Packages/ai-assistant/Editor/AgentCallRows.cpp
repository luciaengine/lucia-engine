#include "AgentCallRows.h"

#include "AgentCallPieces.h"
#include "AssistantActionLedger.h"
#include "AssistantTools.h"
#include "AssistantTurnUndo.h"
#include "ConversationMessage.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{
namespace
{
using Mark = AgentCallRowModel::Mark;

// The thread entry markup_comment adds is not an undo step, by design.
constexpr std::string_view kCommentTool = "markup_comment";

// A tool's bare name, as the ledger records it; a name the table does not have as given.
std::string_view BareName(std::string_view name)
{
    const AssistantTool* tool = AssistantTools::Find(name);
    return tool ? tool->Name : name;
}

std::string ActionWords(std::string_view name)
{
    const AssistantTool* tool = AssistantTools::Find(name);
    return std::string(tool ? tool->Action : name);
}

Mark MarkOf(const AssistantAction& action)
{
    switch (action.State)
    {
    case AssistantActionState::Waiting:
        return Mark::Waiting;
    case AssistantActionState::Done:
        return action.Class == AssistantToolClass::Read || action.Class == AssistantToolClass::View ? Mark::ViewOnly
                                                                                                  : Mark::Done;
    case AssistantActionState::Failed:
    case AssistantActionState::Refused:
    case AssistantActionState::Declined:
    case AssistantActionState::Cancelled:
        break;
    }
    return Mark::Refused;
}

// What the row adds after the subject: why a call did not run, what a waiting call is, or
// that it ran because the user allowed it.
// "(refused)" when the gate said no, "(failed)" when the editor answered with an error.
std::string Outcome(const AssistantAction& action)
{
    switch (action.State)
    {
    case AssistantActionState::Failed:
        return "(failed) " + action.Message;
    case AssistantActionState::Refused:
        return "(refused) " + action.Message;
    case AssistantActionState::Cancelled:
        return action.Message;
    case AssistantActionState::Declined:
        return "you declined";
    case AssistantActionState::Waiting:
        return action.Class == AssistantToolClass::Gated ? "not an undo step" : std::string();
    case AssistantActionState::Done:
        break;
    }
    // A call that ran because the user allowed it says so, as a declined one does.
    switch (action.Answer)
    {
    case AssistantAnswer::Allow:
        return "you allowed";
    case AssistantAnswer::AllowForTurn:
        return "allowed for this turn";
    case AssistantAnswer::None:
    case AssistantAnswer::Deny:
        break;
    }
    return {};
}

// The words a row adds for a call the editor never received.
constexpr std::string_view kRefusedByCli = "(refused) Not allowed for the assistant";

// Where a call's undo step is now.
enum class StepPlace
{
    None,
    Undo,
    Redo,
    Gone,
};

StepPlace PlaceOf(const AssistantAction& action, const AssistantUndoSnapshot& history)
{
    if (action.UndoEntryId == 0)
        return StepPlace::None;
    const auto holds = [&action](const std::vector<uint64_t>& stack) {
        return std::find(stack.begin(), stack.end(), action.UndoEntryId) != stack.end();
    };
    if (holds(history.Undo))
        return StepPlace::Undo;
    return holds(history.Redo) ? StepPlace::Redo : StepPlace::Gone;
}

std::string UndoState(const AssistantAction& action, const AssistantUndoSnapshot& history)
{
    if (action.State == AssistantActionState::Waiting)
        return {};
    if (action.State != AssistantActionState::Done)
        return "nothing changed";
    switch (PlaceOf(action, history))
    {
    case StepPlace::Undo:
        return "Undo step: " + action.UndoName;
    case StepPlace::Redo:
        return "undone";
    case StepPlace::Gone:
        return "no longer in the undo history";
    case StepPlace::None:
        break;
    }
    if (action.Tool == kCommentTool)
        return "a comment stays in the thread";
    switch (action.Class)
    {
    case AssistantToolClass::Read:
        return {};
    case AssistantToolClass::UndoableEdit:
        return "nothing changed";
    case AssistantToolClass::View:
    case AssistantToolClass::Input:
    case AssistantToolClass::Gated:
    case AssistantToolClass::Denied:
        break;
    }
    return "not an undo step";
}

std::string Join(const std::string& first, const std::string& second, std::string_view separator)
{
    if (first.empty())
        return second;
    if (second.empty())
        return first;
    return first + std::string(separator) + second;
}

// Appends `text` pretty-printed (AgentCallJsonLines), or as it is when it is not JSON (a
// response cut at the ledger's limit).
void AppendJsonLines(std::vector<AgentCallPieces>& lines, std::string_view text)
{
    const nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
    if (value.is_discarded())
    {
        lines.push_back({AgentCallPiece::Words(std::string(text))});
        return;
    }
    for (AgentCallPieces& line : AgentCallJsonLines(value))
        lines.push_back(std::move(line));
}

// The raw call and the editor's answer, pretty-printed.
std::vector<AgentCallPieces> DetailLines(std::string_view tool, std::string_view arguments, const AssistantAction* action)
{
    std::vector<AgentCallPieces> lines{{AgentCallPiece::MutedWords("Call"), AgentCallPiece::Words(std::string(tool))}};
    AppendJsonLines(lines, arguments);
    if (!action)
        return lines;
    if (!action->Result.empty())
    {
        lines.push_back({AgentCallPiece::MutedWords("Result")});
        AppendJsonLines(lines, action->Result);
    }
    else if (!action->Message.empty())
    {
        lines.push_back({AgentCallPiece::MutedWords("Answer"), AgentCallPiece::Words(action->Message)});
    }
    return lines;
}

// The row's summary: the call's subject, then why it did not run or what it waits as.
AgentCallPieces SummaryOf(const AssistantAction& action)
{
    AgentCallPieces summary = AgentCallSummary(action);
    if (std::string outcome = Outcome(action); !outcome.empty())
    {
        if (!summary.empty())
            summary.push_back(AgentCallPiece::MutedWords("·"));
        summary.push_back(AgentCallPiece::Words(std::move(outcome)));
    }
    return summary;
}

AgentCallRowModel RowOf(const AssistantAction& action, AgentCallMedia media, const AssistantUndoSnapshot& history)
{
    AgentCallRowModel row;
    row.Glyph = MarkOf(action);
    row.Action = ActionWords(action.Tool);
    row.Summary = SummaryOf(action);
    row.UndoState = UndoState(action, history);
    row.Details = DetailLines(action.Tool, action.Arguments, &action);
    row.RequestId = action.RequestId;
    row.Image = std::move(media.Image);
    row.Resources = std::move(media.Resources);
    if (action.State == AssistantActionState::Waiting)
    {
        row.Asks = action.WaitingFor == AssistantWaitReason::Answer;
        row.Waiting = row.Asks ? "Waiting for your answer" : "Waiting for your edit to finish";
    }
    return row;
}

// The calls of `streamCalls` the editor has no record of in turn `turn`, matched by tool
// and order against the ledger's calls of the turn.
std::vector<const ToolCall*> UnreceivedCalls(std::span<const ToolCall> streamCalls,
                                             std::span<const AssistantAction> actions, uint64_t turn)
{
    std::unordered_map<std::string_view, size_t> recorded;
    for (const AssistantAction& action : actions)
        if (action.Turn == turn)
            ++recorded[action.Tool];
    std::vector<const ToolCall*> unreceived;
    for (const ToolCall& call : streamCalls)
    {
        if (const auto it = recorded.find(BareName(call.Name)); it != recorded.end() && it->second > 0)
        {
            --it->second;
            continue;
        }
        unreceived.push_back(&call);
    }
    return unreceived;
}

// A row for a call the editor never received: the CLI refused it, or it failed before or
// outside the editor ("(failed)" and the first line of its error).
AgentCallRowModel RowOfUnreceived(const ToolCall& call)
{
    const std::string_view name = BareName(call.Name);
    AgentCallRowModel row;
    row.Action = ActionWords(name);
    row.Summary = AgentCallSummary(name, call.InputJson);
    if (call.End != ToolCallEnd::Unknown)
    {
        if (!row.Summary.empty())
            row.Summary.push_back(AgentCallPiece::MutedWords("·"));
        const std::string outcome = call.End == ToolCallEnd::Refused ? std::string(kRefusedByCli)
                                    : call.Error.empty()                ? std::string("(failed)")
                                                                        : "(failed) " + call.Error;
        row.Summary.push_back(AgentCallPiece::Words(outcome));
        row.Glyph = Mark::Refused;
        row.UndoState = "nothing changed";
    }
    row.Details = DetailLines(call.Name, call.InputJson, nullptr);
    if (!call.Error.empty())
        row.Details.push_back({AgentCallPiece::MutedWords("Answer"), AgentCallPiece::Words(call.Error)});
    return row;
}

// The names of a call's arguments: calls of one tool fold only when they name the same.
std::string ArgumentShape(const AssistantAction& action)
{
    const nlohmann::json arguments = nlohmann::json::parse(action.Arguments, nullptr, false);
    std::string shape;
    if (arguments.is_object())
        for (const auto& [key, value] : arguments.items())
            shape += key + ",";
    return shape;
}

// `next` folds into the row that shows `last`: both ran, one tool, the same arguments
// named, and their steps (if any) in the same place. A call with an image or assets
// (AgentCallMedia) keeps a row of its own; the caller checks that.
bool Folds(const AssistantAction& last, const AssistantAction& next, const AssistantUndoSnapshot& history)
{
    return last.State == AssistantActionState::Done && next.State == AssistantActionState::Done &&
           last.Tool == next.Tool && last.Answer == next.Answer && PlaceOf(last, history) == PlaceOf(next, history) &&
           ArgumentShape(last) == ArgumentShape(next);
}

// Shows `calls` consecutive calls in `row`, the last of them `next`; `subjects` holds
// the summaries of the calls before it, comma-separated.
void Fold(AgentCallRowModel& row, AgentCallPieces& subjects, AgentCallPieces& lastSubject, const AssistantAction& next,
          size_t calls, const AssistantUndoSnapshot& history)
{
    // A subject already listed (the same file saved twice) is not repeated.
    const AgentCallPieces summary = AgentCallSummary(next);
    if (summary != lastSubject)
    {
        if (!subjects.empty() && !summary.empty())
            subjects.push_back(AgentCallPiece::MutedWords(","));
        subjects.insert(subjects.end(), summary.begin(), summary.end());
        lastSubject = summary;
    }
    row.Summary = subjects;
    row.Summary.push_back(AgentCallPiece::MutedWords("(" + std::to_string(calls) + " calls)"));
    if (const std::string outcome = Outcome(next); !outcome.empty())
    {
        row.Summary.push_back(AgentCallPiece::MutedWords("·"));
        row.Summary.push_back(AgentCallPiece::Words(outcome));
    }
    if (PlaceOf(next, history) == StepPlace::Undo)
        row.UndoState = std::to_string(calls) + " undo steps";
    std::vector<AgentCallPieces> details = DetailLines(next.Tool, next.Arguments, &next);
    row.Details.push_back({});
    row.Details.insert(row.Details.end(), std::make_move_iterator(details.begin()),
                       std::make_move_iterator(details.end()));
}
} // namespace

std::vector<AgentCallRowModel> AgentCallRowModel::For(std::span<const ToolCall> streamCalls,
                                                      std::span<const AssistantAction> actions, uint64_t turn,
                                                      const AssistantUndoSnapshot& history)
{
    std::vector<AgentCallRowModel> rows;
    // The call whose row takes folds; null after a row with media, which takes none.
    const AssistantAction* folding = nullptr;
    AgentCallPieces foldedSubjects;
    AgentCallPieces lastSubject;
    size_t folded = 0;
    for (const AssistantAction& action : actions)
    {
        if (action.Turn != turn)
            continue;
        AgentCallMedia media = AgentCallMediaOf(action);
        const bool showsMedia = !media.Empty();
        if (folding && !showsMedia && Folds(*folding, action, history))
        {
            Fold(rows.back(), foldedSubjects, lastSubject, action, ++folded, history);
            continue;
        }
        rows.push_back(RowOf(action, std::move(media), history));
        folding = showsMedia ? nullptr : &action;
        foldedSubjects = AgentCallSummary(action);
        lastSubject = foldedSubjects;
        folded = 1;
    }
    for (const ToolCall* call : UnreceivedCalls(streamCalls, actions, turn))
        rows.push_back(RowOfUnreceived(*call));
    return rows;
}

std::string AgentCallRowModel::Counts(std::span<const ToolCall> streamCalls, std::span<const AssistantAction> actions,
                                      uint64_t turn)
{
    size_t steps = 0;
    size_t refused = 0;
    size_t failed = 0;
    size_t declined = 0;
    size_t cancelled = 0;
    for (const AssistantAction& action : actions)
    {
        if (action.Turn != turn)
            continue;
        steps += action.UndoEntryId != 0 ? 1 : 0;
        refused += action.State == AssistantActionState::Refused ? 1 : 0;
        failed += action.State == AssistantActionState::Failed ? 1 : 0;
        declined += action.State == AssistantActionState::Declined ? 1 : 0;
        cancelled += action.State == AssistantActionState::Cancelled ? 1 : 0;
    }
    for (const ToolCall* call : UnreceivedCalls(streamCalls, actions, turn))
    {
        refused += call->End == ToolCallEnd::Refused ? 1 : 0;
        failed += call->End == ToolCallEnd::Failed ? 1 : 0;
    }
    std::string counts;
    if (steps > 0)
        counts = std::to_string(steps) + (steps == 1 ? " undo step" : " undo steps");
    if (refused > 0)
        counts = Join(counts, std::to_string(refused) + " refused", " · ");
    if (failed > 0)
        counts = Join(counts, std::to_string(failed) + " failed", " · ");
    if (declined > 0)
        counts = Join(counts, std::to_string(declined) + " declined", " · ");
    if (cancelled > 0)
        counts = Join(counts, std::to_string(cancelled) + " cancelled", " · ");
    return counts;
}
} // namespace GameEngine
