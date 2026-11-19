#pragma once

#include "Ocean/OceanCpuTexture.h"

#include "AssetCore/GUID.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

#include <memory>

namespace GameEngine
{
class TextureAsset;
struct TexturePixels;
} // namespace GameEngine

namespace GameEngine::Ocean
{

/// The CPU copies of the author maps (flow maps, wave masks) that the ocean reads from texture
/// assets. A copy is converted one time for each payload of its texture and shared with the
/// readers, so an update that reads the same texture again takes no pixels and makes no copy.
class OceanCpuTextureCache
{
  public:
    /// The RG copy of `texture`, which the asset `guid` names. Converts it when the cache has no
    /// copy of this payload (TextureAsset::GetPayloadGeneration). Null when the texture gives no
    /// valid copy: the next call tries again.
    std::shared_ptr<const OceanCpuTextureRG> Get(const GUID& guid, const TextureAsset& texture);

    /// Drops the copies that no Get asked for since the last call. Call one time for each update.
    void DropUnused();

    /// The RG copy of the pixels of a texture (TextureAsset::AcquirePixels), with no cache. Not
    /// valid when the texture has no pixels or a channel size that is not 8, 16 or 32 bits.
    static OceanCpuTextureRG Convert(const TexturePixels& texture);

  private:
    struct Entry
    {
        uint64 PayloadGeneration = 0;
        bool Used = false;
        std::shared_ptr<const OceanCpuTextureRG> Copy;
    };

    FlatMap<GUID, Entry> m_Entries;
};

} // namespace GameEngine::Ocean
