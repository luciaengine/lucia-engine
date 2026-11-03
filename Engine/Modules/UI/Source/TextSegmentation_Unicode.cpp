#include "UI/TextSegmentation.h"
#include "TextSegmentation_Unicode.h"
#include <string>
#include <algorithm>
#include <limits>
#include <memory>
#include <unicode/ubrk.h>
#include <unicode/utext.h>

namespace GameEngine::TextSegmentation
{
namespace
{
size_t GraphemeBoundary(std::string_view text, size_t byteIndex, bool next)
{
    byteIndex = std::min(byteIndex, text.size());
    if (text.empty() || (next && byteIndex == text.size()) || (!next && byteIndex == 0))
        return byteIndex;
    // Keep the usual ASCII edit allocation-free, except CRLF, whose two bytes
    // form one extended grapheme. Non-ASCII always uses ICU's current rules.
    if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x80; }))
    {
        if (next)
            return byteIndex + (text[byteIndex] == '\r' && byteIndex + 1 < text.size() && text[byteIndex + 1] == '\n' ? 2 : 1);
        return byteIndex - (byteIndex >= 2 && text[byteIndex - 2] == '\r' && text[byteIndex - 1] == '\n' ? 2 : 1);
    }
    if (text.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return byteIndex;
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<UText, decltype(&utext_close)> input(
        utext_openUTF8(nullptr, text.data(), static_cast<int64_t>(text.size()), &status), &utext_close);
    std::unique_ptr<UBreakIterator, decltype(&ubrk_close)> iterator(
        ubrk_open(UBRK_CHARACTER, "root", nullptr, 0, &status), &ubrk_close);
    if (U_SUCCESS(status)) ubrk_setUText(iterator.get(), input.get(), &status);
    if (U_FAILURE(status)) return byteIndex;
    const int32_t boundary = next ? ubrk_following(iterator.get(), static_cast<int32_t>(byteIndex))
                                  : ubrk_preceding(iterator.get(), static_cast<int32_t>(byteIndex));
    return boundary == UBRK_DONE ? (next ? text.size() : 0) : static_cast<size_t>(boundary);
}
}

size_t PrevGrapheme(std::string_view text, size_t byteIndex)
{
    return GraphemeBoundary(text, byteIndex, false);
}

size_t NextGrapheme(std::string_view text, size_t byteIndex)
{
    return GraphemeBoundary(text, byteIndex, true);
}
}

namespace GameEngine::TextSegmentation::Detail
{
std::optional<std::vector<WordSegment>> UnicodeWordSegments(std::string_view text)
{
    if (text.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x80; }))
        return std::nullopt;
    // Preserve editor punctuation behavior without maintaining Unicode tables:
    // dots join numbers but split identifiers; colons split on both sides.
    // Every replacement has the same UTF-8 length, preserving public offsets.
    std::string tailored(text);
    for (char& c : tailored)
    {
        if (c == '.') c = ',';
        else if (c == ':') c = '*';
    }
    for (const auto& replacement : {std::pair{"．", "，"}, std::pair{"：", "＊"}, std::pair{"﹕", "﹡"}})
    {
        size_t position = 0;
        while ((position = tailored.find(replacement.first, position)) != std::string::npos)
        {
            tailored.replace(position, 3, replacement.second);
            position += 3;
        }
    }
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<UText, decltype(&utext_close)> input(
        utext_openUTF8(nullptr, tailored.data(), static_cast<int64_t>(tailored.size()), &status), &utext_close);
    std::unique_ptr<UBreakIterator, decltype(&ubrk_close)> iterator(
        ubrk_open(UBRK_WORD, "root", nullptr, 0, &status), &ubrk_close);
    if (U_SUCCESS(status)) ubrk_setUText(iterator.get(), input.get(), &status);
    if (U_FAILURE(status)) return std::nullopt;
    std::vector<WordSegment> segments;
    int32_t start = ubrk_first(iterator.get());
    for (int32_t end = ubrk_next(iterator.get()); end != UBRK_DONE; end = ubrk_next(iterator.get()))
    {
        segments.push_back({{static_cast<size_t>(start), static_cast<size_t>(end)},
            ubrk_getRuleStatus(iterator.get()) >= UBRK_WORD_NUMBER});
        start = end;
    }
    return segments;
}
}

namespace GameEngine::TextSegmentation::Detail
{
size_t VisualWordBoundary(std::string_view text, size_t current, bool right,
                          std::span<const float> caretPositions)
{
    current = std::min(current, text.size());
    const auto segments = UnicodeWordSegments(text);
    if (!segments || caretPositions.size() != text.size() + 1)
        return right ? NextBoundary(text, current) : PrevBoundary(text, current);
    const float origin = caretPositions[current];
    size_t destination = current;
    float distance = std::numeric_limits<float>::infinity();
    const auto consider = [&](size_t boundary)
    {
        const float delta = (caretPositions[boundary] - origin) * (right ? 1.0f : -1.0f);
        if (delta > 0.0f && delta < distance)
        {
            destination = boundary;
            distance = delta;
        }
    };
    consider(0);
    for (const auto& segment : *segments)
        if (segment.Word) consider(segment.Range.Begin);
    consider(text.size());
    return destination;
}
}
