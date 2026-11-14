#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine
{

/// Standard Base64 (RFC 4648, `+` and `/`, `=` padding) of `length` bytes at `data`.
inline std::string Base64Encode(const uint8_t* data, size_t length)
{
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    for (size_t i = 0; i < length; i += 3)
    {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < length)
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < length)
            n |= static_cast<uint32_t>(data[i + 2]);

        result.push_back(kTable[(n >> 18) & 0x3F]);
        result.push_back(kTable[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < length) ? kTable[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < length) ? kTable[n & 0x3F] : '=');
    }
    return result;
}

/// Base64 of the bytes a string holds.
inline std::string Base64Encode(std::string_view bytes)
{
    return Base64Encode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

} // namespace GameEngine
