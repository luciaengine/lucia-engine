#pragma once
#include "UI/TextSegmentation.h"
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::TextSegmentation::Detail
{
struct WordSegment
{
    ByteRange Range;
    bool Word = false;
};
// No value for ASCII (the established fast path) or an ICU failure.
std::optional<std::vector<WordSegment>> UnicodeWordSegments(std::string_view text);
size_t VisualWordBoundary(std::string_view text, size_t current, bool right,
                          std::span<const float> caretPositions);
}
