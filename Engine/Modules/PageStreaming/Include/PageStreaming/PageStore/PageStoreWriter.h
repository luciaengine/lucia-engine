#pragma once

#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
class ByteBuffer;
}

namespace GameEngine::PageStreaming
{

struct EncodedPage;

/// Assembles a store in a temporary sibling; Finish publishes it by rename.
/// Concurrent WritePage/ReadPage calls are allowed. Other operations require no page call in flight.
/// Destruction before Finish removes the temporary file.
class PageStoreWriter
{
public:
    PageStoreWriter() = default;
    ~PageStoreWriter();

    PageStoreWriter(const PageStoreWriter&) = delete;
    PageStoreWriter& operator=(const PageStoreWriter&) = delete;

    /// Starts a new store with `layout` (MakePageStoreLayout) for `target`. Every present page must
    /// be written before Finish; pages go into the file in the order they are written. Returns an empty string, else the reason.
    std::string Create(const std::filesystem::path& target, PageStoreLayout layout);

    /// Rewrites the store at `existing` (validated layout `layout`) as `target` under the key in `layout.Header`,
    /// by rename, not copy: a page not written keeps its bytes. Pages that `layout` adds (AddPresentPages) are
    /// written before Finish. Abandon discards the store. Returns an empty string, else the reason.
    std::string OpenForPatch(const std::filesystem::path& existing, const std::filesystem::path& target,
                             PageStoreLayout layout);

    /// Adds the pages `present` marks to the store being written, unwritten, and moves the pages under
    /// the longer index to the end of the store. Not while a page call runs. Returns an empty string,
    /// else the reason (the store is dropped).
    std::string AddPresentPages(std::span<const uint8> present);

    const PageStoreLayout& Layout() const { return m_Layout; }

    /// Writes a present page and its entry, appending when its stored size changes.
    /// Returns false on an invalid address, a byte count inconsistent with the format/flags, or I/O failure.
    bool WritePage(const PageAddress& address, const EncodedPage& page);

    /// Reads back the stored bytes and the entry of a present page already written (or kept by a
    /// patch). False, with `outBytes` empty, when the page is absent, not written yet
    /// (`outEntry.Offset` 0) or the read fails.
    bool ReadPage(const PageAddress& address, ByteBuffer& outBytes, PageIndexEntry& outEntry) const;

    /// Writes the header and index and publishes the store at its target. Returns an empty
    /// string, else the reason (the temporary file is removed).
    std::string Finish();

    /// Drops the store being written and removes its temporary file.
    void Abandon();

private:
    /// Moves the pages under the index of m_Layout to the end of the store. False on an I/O failure.
    bool MovePagesUnderHead();
    bool WriteAt(uint64 offset, const void* data, std::size_t bytes);
    bool WriteAtLocked(uint64 offset, const void* data, std::size_t bytes);
    bool ReadAt(uint64 offset, void* data, std::size_t bytes);
    bool ReadAtLocked(uint64 offset, void* data, std::size_t bytes) const;

    std::filesystem::path m_Target;
    std::filesystem::path m_Temp;
    std::FILE* m_File = nullptr;
    // One file position: every seek + transfer holds it, and every write of an entry's fields.
    mutable std::mutex m_FileMutex;
    PageStoreLayout m_Layout;
    uint64 m_End = 0; // the file's end: where the next appended page goes; under m_FileMutex
    bool m_Failed = false;
};

} // namespace GameEngine::PageStreaming
