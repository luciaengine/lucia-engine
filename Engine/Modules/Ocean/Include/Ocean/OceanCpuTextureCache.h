#pragma once

#include "Ocean/OceanCpuTexture.h"

#include "AssetCore/GUID.h"
#include "Types/FlatMap.h"
#include "Types/Types.h"

namespace GameEngine
{
class TextureAsset;
} // namespace GameEngine

namespace GameEngine::Ocean
{

/// The CPU copies of the author maps (flow maps, wave masks) that the ocean reads from texture
/// assets. A copy is converted one time for each payload of its texture, so an update that reads
/// the same texture again takes no pixels from the asset.
class OceanCpuTextureCache
{
  public:
    /// The RG copy of `texture`, which the asset `guid` names. Converts it when the cache has no
    /// copy of this payload (TextureAsset::GetPayloadGeneration). Valid until the next call.
    const OceanCpuTextureRG& Get(const GUID& guid, const TextureAsset& texture);

    /// Drops the copies that no Get asked for since the last call. Call one time for each update.
    void DropUnused();

    /// The RG copy of `texture`, with no cache. Not valid when the texture has no pixels or a
    /// channel size that is not 8, 16 or 32 bits.
    static OceanCpuTextureRG Convert(const TextureAsset& texture);

  private:
    struct Entry
    {
        const TextureAsset* Texture = nullptr; // identity only: another instance can take the GUID
        uint32 PayloadGeneration = 0;
        bool Used = false;
        OceanCpuTextureRG Copy;
    };

    FlatMap<GUID, Entry> m_Entries;
};

} // namespace GameEngine::Ocean
