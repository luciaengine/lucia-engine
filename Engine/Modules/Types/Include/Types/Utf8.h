#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace GameEngine
{

/// True when `text` is well-formed UTF-8: no stray continuation byte, no truncated sequence, no
/// overlong form, no surrogate code point and nothing above U+10FFFF.
inline bool IsValidUtf8(std::string_view text)
{
    size_t at = 0;
    while (at < text.size())
    {
        const auto lead = static_cast<uint8_t>(text[at]);
        size_t length = 0;
        uint32_t codepoint = 0;
        if (lead < 0x80)
        {
            ++at;
            continue;
        }
        if ((lead & 0xE0) == 0xC0)
        {
            length = 2;
            codepoint = lead & 0x1F;
        }
        else if ((lead & 0xF0) == 0xE0)
        {
            length = 3;
            codepoint = lead & 0x0F;
        }
        else if ((lead & 0xF8) == 0xF0)
        {
            length = 4;
            codepoint = lead & 0x07;
        }
        else
        {
            return false;
        }
        if (at + length > text.size())
            return false;
        for (size_t i = 1; i < length; ++i)
        {
            const auto next = static_cast<uint8_t>(text[at + i]);
            if ((next & 0xC0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        constexpr uint32_t kSmallestForLength[] = {0, 0, 0x80, 0x800, 0x10000};
        if (codepoint < kSmallestForLength[length] || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF))
            return false;
        at += length;
    }
    return true;
}

/// The longest prefix of UTF-8 `text` that holds at most `maxBytes` bytes and does not split a
/// character: `maxBytes` moved back past any continuation bytes it would cut.
inline size_t Utf8PrefixLength(std::string_view text, size_t maxBytes)
{
    if (maxBytes >= text.size())
        return text.size();
    size_t end = maxBytes;
    while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80)
        --end;
    return end;
}

} // namespace GameEngine
