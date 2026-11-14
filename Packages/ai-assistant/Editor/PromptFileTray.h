#pragma once

#include "PromptFiles.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{

class UIElement;

/// The files picked for the next prompt and the strip above the prompt input that shows them:
/// the + button and a drop add files, an item's x removes its file, and a sent prompt takes
/// them all. Each file is read (PromptFiles::Load, which can decode and scale a large image) on
/// a background job; its item shows "loading" until the read lands on the UI thread, then the
/// file's name and, for an image, its thumbnail.
class PromptFileTray
{
public:
    /// `strip` is hidden while the tray is empty; `items` holds one item per file and posts the
    /// reads' results back to the UI thread. `jobs` runs the reads. `status` shows why a picked
    /// file was not added.
    PromptFileTray(UIElement& strip, UIElement& items, JobSystem::WorkStealingThreadPool& jobs,
                   std::function<void(const std::string&)> status);
    ~PromptFileTray();

    /// Starts reading each path that is not in the tray yet. A path past PromptFiles::kMaxFiles
    /// and every read that is refused are named in the status line, joined in one message once
    /// the call's reads have all landed.
    void Add(std::span<const std::filesystem::path> paths);

    /// Opens the file dialog in the project folder and adds the files the user picks.
    void Pick();

    /// The files read so far, in the order they were added.
    std::vector<PromptFile> Files() const;

    /// A file is still being read; the prompt waits for it.
    bool IsLoading() const;

    /// Empties the tray after the prompt that carried its files was sent.
    void Clear();

private:
    /// One file in the tray: being read while File is empty.
    struct Entry
    {
        std::filesystem::path Path;
        std::optional<PromptFile> File;
    };

    /// The refusals of one Add call, in the order of its paths, shown together once its last
    /// read lands. A read's slot stays empty when it is not refused.
    struct Batch
    {
        std::vector<std::string> Refusals;
        std::size_t Pending = 0;
    };

    void Finish(const std::filesystem::path& path, PromptFileLoad load, Batch& batch, std::size_t slot);
    void ShowRefusals(const Batch& batch);
    void Remove(const std::filesystem::path& path);
    void ShowItems();

    UIElement& m_Strip;
    UIElement& m_Items;
    JobSystem::WorkStealingThreadPool& m_Jobs;
    std::function<void(const std::string&)> m_Status;
    std::vector<Entry> m_Entries;
    /// Expires with the tray, so a read that lands after it is gone is dropped.
    std::shared_ptr<int> m_Alive = std::make_shared<int>(0);
};

} // namespace GameEngine
