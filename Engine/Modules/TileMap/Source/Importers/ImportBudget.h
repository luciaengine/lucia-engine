#pragma once

#include "TileMap/Importers/TileMapImportResult.h"

#include <filesystem>

namespace GameEngine::TileMap::Importers
{

// One budget is owned by the import context and shared by every external file,
// level and layer. Source-byte limits precede parsing; cell and chunk limits
// precede decoding and allocation, including sparse one-cell-per-chunk maps.
enum class SourceFileCharge
{
    Charged,
    NotARegularFile, // missing, unreadable, a directory, a FIFO or a device
    OverBudget,
};

class ImportBudget
{
public:
    // Charges a source file's size before it is read. Anything but a regular
    // file is refused here: a FIFO or a device has no size and would block the
    // reader that follows.
    SourceFileCharge ReadFile(const std::filesystem::path& path)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            return SourceFileCharge::NotARegularFile;
        const auto bytes = std::filesystem::file_size(path, error);
        if (error)
            return SourceFileCharge::NotARegularFile;
        return Consume(bytes, m_SourceBytes, kMaxImportSourceBytes) ? SourceFileCharge::Charged
                                                                     : SourceFileCharge::OverBudget;
    }

    bool Items(uint64 count) { return Consume(count, m_Items, kMaxImportItems); }
    bool Cells(uint64 count) { return Consume(count, m_Cells, kMaxImportCells); }
    bool Tiles(uint64 count) { return Consume(count, m_Tiles, kMaxImportTilesPerSet); }
    bool Chunk() { return Consume(1, m_Chunks, kMaxImportOutputChunks); }

private:
    static bool Consume(uint64 count, uint64& used, uint64 limit)
    {
        if (count > limit - used)
            return false;
        used += count;
        return true;
    }

    uint64 m_SourceBytes = 0;
    uint64 m_Items = 0;
    uint64 m_Cells = 0;
    uint64 m_Tiles = 0;
    uint64 m_Chunks = 0;
};

} // namespace GameEngine::TileMap::Importers
