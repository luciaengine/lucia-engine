#include "PromptFileTray.h"

#include "PromptFileItem.h"

#include "Editor/EditorPaths.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/Shell.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UI/UiPostHandle.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace GameEngine
{

PromptFileTray::PromptFileTray(UIElement& strip, UIElement& items, JobSystem::WorkStealingThreadPool& jobs,
                               std::function<void(const std::string&)> status)
    : m_Strip(strip)
    , m_Items(items)
    , m_Jobs(jobs)
    , m_Status(std::move(status))
{
    ShowItems();
}

PromptFileTray::~PromptFileTray() = default;

void PromptFileTray::Add(std::span<const std::filesystem::path> paths)
{
    auto batch = std::make_shared<Batch>();
    const UI::UiPostHandle post = m_Items.GetPostHandle();
    std::vector<std::pair<std::filesystem::path, std::size_t>> reads;
    for (const std::filesystem::path& path : paths)
    {
        const bool present = std::any_of(m_Entries.begin(), m_Entries.end(),
                                         [&path](const Entry& entry) { return entry.Path == path; });
        if (present)
            continue;
        if (m_Entries.size() >= PromptFiles::kMaxFiles)
        {
            batch->Refusals.push_back(PathUtf8(path.filename()) + " was not added: a prompt carries at most " +
                                      std::to_string(PromptFiles::kMaxFiles) + " files");
            continue;
        }
        m_Entries.push_back({path, std::nullopt});
        const std::size_t slot = batch->Refusals.size();
        batch->Refusals.emplace_back();
        ++batch->Pending;
        reads.emplace_back(path, slot);
    }
    // Instantiate the staged chip layout before image jobs occupy the pool: its first load
    // can need an asset job, and must not wait behind a large image decode on the UI thread.
    ShowItems();
    for (const auto& [path, slot] : reads)
    {
        m_Jobs.EnqueueWork(
            [this, path, post, batch, slot, alive = std::weak_ptr<int>(m_Alive)]()
            {
                auto load = std::make_shared<PromptFileLoad>(PromptFiles::Load(path));
                post.Post([this, path, load, batch, slot, alive]()
                {
                    if (!alive.expired())
                        Finish(path, std::move(*load), *batch, slot);
                });
            },
            JobSystem::JobPriority::Background);
    }
    ShowRefusals(*batch);
}

void PromptFileTray::Pick()
{
    const std::vector<std::filesystem::path> paths =
        Platform::SelectFiles(Editor::GetCurrentEditorProjectPaths().projectRoot, "All Files", "*.*");
    Add(paths);
}

std::vector<PromptFile> PromptFileTray::Files() const
{
    std::vector<PromptFile> files;
    for (const Entry& entry : m_Entries)
        if (entry.File)
            files.push_back(*entry.File);
    return files;
}

bool PromptFileTray::IsLoading() const
{
    return std::any_of(m_Entries.begin(), m_Entries.end(), [](const Entry& entry) { return !entry.File; });
}

void PromptFileTray::Clear()
{
    if (m_Entries.empty())
        return;
    m_Entries.clear();
    ShowItems();
}

void PromptFileTray::Finish(const std::filesystem::path& path, PromptFileLoad load, Batch& batch, std::size_t slot)
{
    --batch.Pending;
    // A file removed while it was being read has no entry left, and its result is dropped.
    const auto entry = std::find_if(m_Entries.begin(), m_Entries.end(),
                                    [&path](const Entry& candidate) { return candidate.Path == path && !candidate.File; });
    if (entry != m_Entries.end())
    {
        if (load.File)
            entry->File = std::move(load.File);
        else
            m_Entries.erase(entry);
        ShowItems();
        batch.Refusals[slot] = std::move(load.Refusal);
    }
    ShowRefusals(batch);
}

void PromptFileTray::ShowRefusals(const Batch& batch)
{
    if (batch.Pending != 0)
        return;
    std::string refusals;
    for (const std::string& refusal : batch.Refusals)
        if (!refusal.empty())
            refusals += (refusals.empty() ? "" : "; ") + refusal;
    if (!refusals.empty())
        m_Status(refusals);
}

void PromptFileTray::Remove(const std::filesystem::path& path)
{
    std::erase_if(m_Entries, [&path](const Entry& entry) { return entry.Path == path; });
    ShowItems();
}

void PromptFileTray::ShowItems()
{
    m_Items.RemoveAllChildren();
    if (m_Entries.empty())
        m_Strip.AddClass("hidden");
    else
        m_Strip.RemoveClass("hidden");
    UIManager* ui = m_Items.GetOwnerManager();
    if (!ui)
        return;
    for (std::size_t index = 0; index < m_Entries.size(); ++index)
    {
        const Entry& entry = m_Entries[index];
        // The items are rebuilt after the click, not in it: the click runs on the button it removes.
        auto item = std::make_unique<PromptFileItem>(
            index, [this, path = entry.Path]() { m_Items.PostSafeAction([this, path]() { Remove(path); }); });
        if (!item->Build(*ui))
            return;
        item->Show(entry.Path, entry.File);
        m_Items.AddChild(std::move(item));
    }
}

} // namespace GameEngine
