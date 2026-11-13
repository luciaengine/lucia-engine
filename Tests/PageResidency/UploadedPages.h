#pragma once

#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageResidency.h"
#include "Types/Types.h"

#include <cstddef>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace GameEngine::PageStreaming::Test
{

/// The bytes of every page a field's cache uploaded, copied after each Update as the field's owner
/// copies them. A stream over planes drops a page's bytes at the Update after its upload.
class UploadedPages
{
public:
    explicit UploadedPages(uint32 field) : m_Field(field) {}

    /// Copies the bytes of this frame's uploads. Call after each Update.
    void Take(const PageResidencyManager& residency)
    {
        for (const PageUploadRequest& upload : residency.FieldCache(m_Field).Uploads())
            if (const std::vector<uint8>* bytes = residency.Bytes(upload.Page.Stream, upload.Page.Address))
                m_Bytes[KeyOf(upload.Page.Stream, upload.Page.Address)] = *bytes;
    }

    /// The bytes of the last upload of page `address` of stream `stream`, or null.
    const std::vector<uint8>* Find(uint32 stream, const PageAddress& address) const
    {
        const auto it = m_Bytes.find(KeyOf(stream, address));
        return it == m_Bytes.end() ? nullptr : &it->second;
    }

    /// The pages of `wanted` of level `level` that stream `stream` uploaded.
    std::size_t Count(uint32 stream, std::span<const PageWant> wanted, uint32 level) const
    {
        std::size_t count = 0;
        for (const PageWant& want : wanted)
            if (want.Address.Level == level && Find(stream, want.Address) != nullptr)
                ++count;
        return count;
    }

private:
    using Key = std::tuple<uint32, uint32, uint32, uint32>;

    static Key KeyOf(uint32 stream, const PageAddress& address)
    {
        return {stream, address.Level, address.X, address.Z};
    }

    uint32 m_Field = 0;
    std::map<Key, std::vector<uint8>> m_Bytes;
};

} // namespace GameEngine::PageStreaming::Test
