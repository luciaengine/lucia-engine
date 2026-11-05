#include "Panels/UndoHistory/UndoHistoryPanel.h"
#include "UndoRedo/UndoRedoService.h"
#include "Types/FlatMap.h"
#include "UI/UIElement.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/UIEvents.h"
#include "AssetCore/SharedFileRead.h"
#include "Editor/EditorPaths.h"
#include "Logger/Logger.h"
#include "UI/Parsers/XMLParser.h"

#include <bitset>
#include <chrono>
#include <ctime>
#include <span>
#include <string>

namespace GameEngine {

namespace {

std::unique_ptr<UIElement> ParseHistoryLayout(const char* path, const std::string& text,
                                              std::span<const std::string_view> requiredIds)
{
    std::unique_ptr<UIElement> root;
    if (!UIParsing::XMLParser::ParseLayoutFromString(text, root, path) || !root)
        return nullptr;
    for (const auto id : requiredIds)
    {
        if (root->FindById(id))
            continue;
        Logger::Log::Error("Undo History: {} requires the element {}", path, id);
        return nullptr;
    }
    return root;
}

std::string ReadHistoryLayout(const char* path)
{
    std::string text;
    const auto file = Editor::GetEditorGlobalPaths().installAssetsRoot / path;
    if (!ReadFileTextShared(file, text))
        Logger::Log::Error("Undo History: cannot read {}", file.string());
    return text;
}

constexpr std::size_t kLaneUp = 0;
constexpr std::size_t kLaneDown = 1;
constexpr std::size_t kLaneLeft = 2;
constexpr std::size_t kLaneRight = 3;
constexpr std::size_t kLaneMark = 4;
constexpr std::size_t kLaneCurrent = 5;
constexpr std::size_t kLanePartCount = 6;

using HistoryLaneParts = std::bitset<kLanePartCount>;

void KeepHistoryLanePart(UIElement& lane, const char* id, bool keep)
{
    auto* part = lane.FindById(id);
    if (keep)
        part->SetId("");
    else
        lane.RemoveChild(part);
}

std::unique_ptr<UIElement> BuildHistoryLane(const HistoryLaneParts& parts)
{
    constexpr std::string_view required[] = {"HistoryLineUp", "HistoryLineDown", "HistoryLineLeft", "HistoryLineRight", "HistoryMark"};
    constexpr const char* path = "UI/controls/UndoHistory/HistoryLane.uxml";
    static const std::string text = ReadHistoryLayout(path);
    auto lane = ParseHistoryLayout(path, text, required);
    if (!lane)
        return nullptr;
    if (parts.test(kLaneCurrent))
        lane->FindById("HistoryMark")->AddClass("history-node-current");
    KeepHistoryLanePart(*lane, "HistoryLineUp", parts.test(kLaneUp));
    KeepHistoryLanePart(*lane, "HistoryLineDown", parts.test(kLaneDown));
    KeepHistoryLanePart(*lane, "HistoryLineLeft", parts.test(kLaneLeft));
    KeepHistoryLanePart(*lane, "HistoryLineRight", parts.test(kLaneRight));
    KeepHistoryLanePart(*lane, "HistoryMark", parts.test(kLaneMark));
    return lane;
}

std::string FormatTimestamp(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{})
        return {};
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char timestamp[sizeof("00:00:00")];
    std::strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tm);
    return timestamp;
}

} // namespace

UndoHistoryPanel::UndoHistoryPanel()
    : DockPanel("Undo History")
{
    BuildUI();
}

UndoHistoryPanel::~UndoHistoryPanel() = default;

void UndoHistoryPanel::SetUndoRedoService(Editor::UndoRedoService* undo)
{
    if (m_Undo == undo)
        return;
    m_HistoryChanged.Reset();
    m_Undo = undo;
    if (m_Undo)
        m_HistoryChanged = m_Undo->HistoryChanged().Subscribe([this] { Rebuild(); });
    Rebuild();
}

void UndoHistoryPanel::BuildUI()
{
    constexpr std::string_view required[] = {"HistoryScroll", "HistoryCount", "HistoryUndo", "HistoryRedo", "HistoryClear"};
    constexpr const char* path = "UI/controls/UndoHistory/UndoHistory.uxml";
    static const std::string text = ReadHistoryLayout(path);
    auto root = ParseHistoryLayout(path, text, required);
    if (!root)
        return;
    root->RequestSubtreeStyleAssetPath("UI/controls/UndoHistory/UndoHistory.css", "editor");
    if (auto* scrollView = dynamic_cast<ScrollView*>(root->FindById("HistoryScroll")))
    {
        auto list = std::make_unique<UIElement>();
        list->SetId("HistoryList");
        list->AddClass("undo-history-list");
        m_List = list.get();
        scrollView->AddContent(std::move(list));
    }
    m_CountLabel = dynamic_cast<Label*>(root->FindById("HistoryCount"));
    m_BackBtn = dynamic_cast<Button*>(root->FindById("HistoryUndo"));
    m_ForwardBtn = dynamic_cast<Button*>(root->FindById("HistoryRedo"));
    auto* clear = dynamic_cast<Button*>(root->FindById("HistoryClear"));
    if (m_BackBtn)
        m_BackBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { if (m_Undo) m_Undo->Undo(); });
    if (m_ForwardBtn)
        m_ForwardBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { if (m_Undo) m_Undo->Redo(); });
    if (clear)
        clear->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { if (m_Undo) m_Undo->Clear(); });
    AddChild(std::move(root));
    Rebuild();
}

void UndoHistoryPanel::AddRevision(std::uint64_t id, const std::string& label, const std::vector<bool>& branches,
                                   bool startsBranch, bool hasChildren, const std::string& tooltip, const std::string& description)
{
    constexpr std::string_view required[] = {"HistoryBranches", "HistoryCurrent", "HistoryContent", "HistoryTitle", "HistoryDetails"};
    constexpr const char* path = "UI/controls/UndoHistory/HistoryRevision.uxml";
    static const std::string text = ReadHistoryLayout(path);
    auto root = ParseHistoryLayout(path, text, required);
    auto* row = dynamic_cast<Button*>(root.get());
    if (!row)
        return;
    const bool current = id == m_Undo->GetCurrentNodeId();
    row->SetId("HistoryNode" + std::to_string(id));
    row->SetText(label);
    row->SetTooltip(description.empty() ? tooltip : tooltip + "\n" + description);
    auto* gutter = row->FindById("HistoryBranches");
    gutter->SetId("");
    row->GetChildren().front()->AddClass("history-button-label");
    auto* title = dynamic_cast<Label*>(row->FindById("HistoryTitle"));
    title->SetText(label);
    auto* content = row->FindById("HistoryContent");
    auto* details = dynamic_cast<Label*>(row->FindById("HistoryDetails"));
    if (description.empty())
        content->RemoveChild(details);
    else
        details->SetText(description);
    for (std::size_t index = 0; index <= branches.size(); ++index)
    {
        const bool nodeLane = index == branches.size();
        const bool elbow = !nodeLane && startsBranch && index + 1 == branches.size();
        HistoryLaneParts parts;
        parts.set(kLaneUp, nodeLane ? id != m_Undo->GetRootNodeId() && !startsBranch : branches[index] || elbow);
        parts.set(kLaneDown, nodeLane ? hasChildren : branches[index]);
        parts.set(kLaneLeft, nodeLane && startsBranch);
        parts.set(kLaneRight, elbow);
        parts.set(kLaneMark, nodeLane);
        parts.set(kLaneCurrent, nodeLane && current);
        auto lane = BuildHistoryLane(parts);
        if (!lane)
            return;
        if (!nodeLane && branches[index])
            lane->AddClass("history-branch-continues");
        if (elbow)
            lane->AddClass("history-branch-elbow");
        gutter->AddChild(std::move(lane));
    }
    if (current)
        row->AddClass("history-current");
    else
        row->RemoveChild(row->FindById("HistoryCurrent"));
    row->RegisterEventHandler(kEventButtonClick, [this, id](UIEvent& event)
    {
        if (m_Undo)
            m_Undo->Checkout(id);
        event.Stop();
    });
    m_List->AddChild(std::move(root));
}

void UndoHistoryPanel::AddNotice(const std::string& text, const std::string& id, const std::string& tooltip)
{
    auto row = std::make_unique<Label>();
    row->SetId(id);
    row->AddClass("history-pending");
    row->SetText(text);
    row->SetTooltip(tooltip);
    m_List->AddChild(std::move(row));
}

void UndoHistoryPanel::OnOwnerManagerChanged(UIManager* owner)
{
    DockPanel::OnOwnerManagerChanged(owner);
    m_RebuildQueued = false;
    if (owner)
        Rebuild();
}

void UndoHistoryPanel::OnMountVisibilityChanged(bool isVisible)
{
    DockPanel::OnMountVisibilityChanged(isVisible);
    // A queued rebuild can be dropped while this panel is unmounted.
    m_RebuildQueued = false;
    if (isVisible)
        Rebuild();
}

void UndoHistoryPanel::Rebuild()
{
    if (!m_List)
        return;
    if (UIElement::IsInEventDispatch())
    {
        if (GetOwnerManager() && !m_RebuildQueued)
            m_RebuildQueued = PostSafeAction([this]
            {
                m_RebuildQueued = false;
                Rebuild();
            });
        return;
    }
    m_List->RemoveAllChildren();

    if (m_Undo)
    {
        FlatMap<std::uint64_t, std::vector<const Editor::HistoryGraph::Node*>> children;
        for (const auto& node : m_Undo->GetHistoryNodes())
            children.GetOrInsert(node.Parent).push_back(&node);
        struct Revision
        {
            const Editor::HistoryGraph::Node* HistoryNode = nullptr;
            std::vector<bool> Branches;
            bool StartsBranch = false;
        };
        std::vector<Revision> revisions{{}};
        while (!revisions.empty())
        {
            Revision revision = std::move(revisions.back());
            revisions.pop_back();
            const auto* node = revision.HistoryNode;
            const std::uint64_t id = node ? node->Id : m_Undo->GetRootNodeId();
            const auto* descendants = children.Find(id);
            const bool hasChildren = descendants && !descendants->empty();
            const std::string label = node ? "#" + std::to_string(id) + " " + node->GetName() : "Starting State";
            const std::string tooltip = node
                ? label + "\nBranch from #" + std::to_string(node->Parent) + " at " + FormatTimestamp(node->Timestamp)
                : "Restore the earliest retained state";
            AddRevision(id, label, revision.Branches, revision.StartsBranch, hasChildren, tooltip,
                node ? node->GetDescription() : std::string{});
            if (!hasChildren)
                continue;
            const bool forks = descendants->size() > 1;
            for (std::size_t child = descendants->size(); child > 0; --child)
            {
                auto branches = revision.Branches;
                if (forks)
                    branches.push_back(child < descendants->size());
                revisions.push_back({(*descendants)[child - 1], std::move(branches), forks});
            }
        }
        if (const char* pending = m_Undo->GetPendingName())
        {
            const std::string description = m_Undo->GetPendingDescription();
            AddNotice(std::string(pending) + " (pending)" + (description.empty() ? "" : "\n" + description), "HistoryPending",
                "Recorded when the next action begins. Undo restores the starting value.");
        }
    }
    if (m_Undo && !m_Undo->GetLastError().empty())
    {
        AddNotice(m_Undo->GetLastError(), "", "");
    }
    if (m_CountLabel)
    {
        const std::size_t count = m_Undo ? m_Undo->GetHistoryNodes().size() : 0;
        m_CountLabel->SetText(std::to_string(count) + (count == 1 ? " Action" : " Actions"));
    }
    if (m_BackBtn)
        m_BackBtn->SetEnabled(m_Undo && m_Undo->CanUndo());
    if (m_ForwardBtn)
        m_ForwardBtn->SetEnabled(m_Undo && m_Undo->CanRedo());
}

} // namespace GameEngine
