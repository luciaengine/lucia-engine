#pragma once

#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class SharedFileReader;
}

namespace GameEngine::PageStreaming
{

/// "GEPG" read as a little-endian uint32.
inline constexpr uint32 kPageStoreMagic = 0x47504547u;
/// magic, version, format, filter, units, faces, face map, samples X, samples Z, level count,
/// reference radius, key.
inline constexpr std::size_t kPageStoreHeaderBytes = 4u + 4u + 5u * 1u + 3u * 4u + 8u + 8u;
static_assert(kPageStoreHeaderBytes == 41u);
/// samples X, samples Z, pages X, pages Z, step.
inline constexpr std::size_t kPageStoreLevelBytes = 4u * 5u;
/// first entry, entry count.
inline constexpr std::size_t kPageStoreRangeBytes = 8u * 2u;
/// The coarsest step of a level: 2^100, which holds any finite float32 range in 32-bit integers under
/// kHeightIntegerMax. A store whose step passes it is refused at load.
inline constexpr int32 kHeightStepExponentMax = 100;
/// The largest magnitude of a stored integer: 2^30. A level's step is never so fine that a height
/// passes it, so a page's integers and their range stay inside int32.
inline constexpr float64 kHeightIntegerMax = 1073741824.0;

/// The refusal of a store or container whose kind, format or filter this engine does not know.
inline constexpr std::string_view kNewerPageStoreRefusal =
    "the page store was written by a newer engine or is damaged: update the engine, or export the game again "
    "(in the editor, opening the scene cooks the terrain again)";

/// Returns the field kind for a supported format/filter pair, otherwise no value.
/// Height, weights, normals, grass control and premultiplied color each have one encoding.
std::optional<PageFieldKind> PageEncodingKind(PageFieldFormat format, PageFieldFilter filter);

/// The name of `kind` as a refusal reads it, for example "grass control".
const char* PageFieldKindName(PageFieldKind kind);

/// The header, level directory, range directory and page index of `layout`, as the store's first
/// DataOffset() bytes.
std::vector<uint8> SerializePageStoreHead(const PageStoreLayout& layout);

/// Validates the requested store kind, version, directories, page ranges and byte extents.
/// Returns an empty string and fills out on success, otherwise a reason with the required fix.
std::string ReadPageStoreHead(SharedFileReader& file, PageFieldKind kind, uint64 baseOffset, uint64 availableBytes,
                              PageStoreLayout& out);

} // namespace GameEngine::PageStreaming
