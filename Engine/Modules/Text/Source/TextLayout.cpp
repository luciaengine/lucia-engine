#include "Rendering/Text/TextLayout.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <unicode/ubrk.h>
#include <unicode/ubidi.h>
#include <unicode/utf8.h>
#include <unicode/utf16.h>
#include <unicode/utext.h>

namespace GameEngine
{
namespace Rendering
{
namespace Text
{

namespace
{
struct StyledPiece
{
    const StyledRun* Run;
    size_t Begin;
    size_t End;
};

std::vector<StyledPiece> OrderStyledLine(const std::vector<StyledRun>& runs, std::string& text)
{
    std::vector<StyledPiece> logical;
    for (const auto& run : runs)
    {
        const auto begin = text.size();
        text.append(run.Text);
        if (text.size() != begin) logical.push_back({&run, begin, text.size()});
    }
    if (text.empty() || text.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x80; }))
        return logical;
    std::vector<UChar> unicode;
    std::vector<size_t> bytes;
    for (int32_t offset = 0; offset < static_cast<int32_t>(text.size());)
    {
        const size_t begin = static_cast<size_t>(offset);
        UChar32 cp;
        U8_NEXT(text.data(), offset, static_cast<int32_t>(text.size()), cp);
        if (cp < 0) cp = 0xfffd;
        bytes.push_back(begin);
        if (cp > 0xffff)
        {
            unicode.push_back(U16_LEAD(cp));
            bytes.push_back(begin);
            unicode.push_back(U16_TRAIL(cp));
        }
        else unicode.push_back(static_cast<UChar>(cp));
    }
    bytes.push_back(text.size());
    UErrorCode error = U_ZERO_ERROR;
    std::unique_ptr<UBiDi, decltype(&ubidi_close)> bidi(ubidi_open(), ubidi_close);
    if (!bidi) return logical;
    ubidi_setPara(bidi.get(), unicode.data(), static_cast<int32_t>(unicode.size()),
                  UBIDI_DEFAULT_LTR, nullptr, &error);
    const auto count = ubidi_countRuns(bidi.get(), &error);
    if (U_FAILURE(error)) return logical;
    std::vector<StyledPiece> visual;
    for (int32_t index = 0; index < count; ++index)
    {
        int32_t start = 0, length = 0;
        const bool rtl = ubidi_getVisualRun(bidi.get(), index, &start, &length) == UBIDI_RTL;
        const auto first = visual.size();
        for (const auto& piece : logical)
        {
            const auto begin = std::max(piece.Begin, bytes[start]);
            const auto end = std::min(piece.End, bytes[start + length]);
            if (begin < end) visual.push_back({piece.Run, begin, end});
        }
        if (rtl) std::reverse(visual.begin() + first, visual.end());
    }
    return visual;
}
}

TextLayout::MultilineShapeResult TextLayout::ShapeMultiline(std::span<const StyledRun> runs,
                                                            float wrapWidth,
                                                            float lineBoxPx,
                                                            WordBreak wordBreak)
{
    MultilineShapeResult out{};

    // Pack float[4] RGBA into uint32_t with R in the low byte — matches the
    // GPU's unpackRGBA8 (ui_sdf_common.glsl). Earlier this packed ARGB, which
    // swapped R/B channels on screen (e.g. Vector3 field X-axis rendered blue
    // instead of red).
    auto packRgba = [](const float c[4]) -> uint32_t {
        auto clampByte = [](float f) -> uint8_t { return (uint8_t)std::min(255.0f, std::max(0.0f, f * 255.0f + 0.5f)); };
        return  (uint32_t)clampByte(c[0])
             | ((uint32_t)clampByte(c[1]) << 8)
             | ((uint32_t)clampByte(c[2]) << 16)
             | ((uint32_t)clampByte(c[3]) << 24);
    };

    auto lineHForRun = [](const StyledRun* run) -> float {
        if (!run || !run->Font) return 0.0f;
        auto lm = run->Font->GetFontLineMetrics(run->PixelSize);
        return std::max(1.0f, lm.ascender + lm.descender);
    };

    struct ShapedLine
    {
        float Height = 0.0f;
        float Advance = 0.0f;
        size_t GlyphStart = 0;
        size_t GlyphEnd = 0;
        // Baseline in the block's Y-down frame — the same space Glyphs[i].y
        // lives in, so emission can snap the line without re-deriving it.
        float Baseline = 0.0f;
    };

    // Shape a single visual line (list of run segments) and append placements to out.
    // lineY is the top of this line in Y-down coordinates.
    auto shapeLine = [&](const std::vector<StyledRun>& lineRuns, float lineY) -> ShapedLine {
        ShapedLine line{};
        line.GlyphStart = out.Glyphs.size();
        float penX = 0.0f;
        // All styled runs share a baseline, including runs with different
        // font sizes. Reserve the largest ascent and descent independently.
        float maxAscender = 0.0f;
        float maxDescender = 0.0f;
        for (const auto& run : lineRuns)
        {
            if (!run.Font || run.Text.empty()) continue;
            const auto metrics = run.Font->GetFontLineMetrics(run.PixelSize);
            maxAscender = std::max(maxAscender, metrics.ascender);
            maxDescender = std::max(maxDescender, metrics.descender);
        }
        line.Height = std::max(1.0f, maxAscender + maxDescender);
        std::string context;
        const auto pieces = OrderStyledLine(lineRuns, context);
        for (const auto& piece : pieces)
        {
            const auto& run = *piece.Run;
            if (!run.Font || run.Text.empty()) continue;
            auto* font = const_cast<FontAtlas*>(run.Font);
            FontAtlas::ShapeResult sr;
            uint32_t rgba = packRgba(run.Color);
            font->ShapeTextRange(context, piece.Begin, piece.End, run.PixelSize, sr, rgba, run.LetterSpacingPx);
            if (sr.atlasChanged) out.AtlasChanged = true;
            auto lm = font->GetFontLineMetrics(run.PixelSize);
            for (auto& gp : sr.glyphs)
            {
                gp.x += penX;
                gp.y += lineY + maxAscender - lm.ascender;
                out.Glyphs.push_back(gp);
            }
            penX += sr.metrics.width;
        }
        line.GlyphEnd = out.Glyphs.size();
        line.Advance = penX;
        const float halfLeading = HalfLeadingPx(line.Height, lineBoxPx);
        if (halfLeading != 0.0f)
        {
            for (size_t i = line.GlyphStart; i < line.GlyphEnd; ++i)
                out.Glyphs[i].y += halfLeading;
        }
        line.Baseline = lineY + halfLeading + maxAscender;
        return line;
    };

    float curY = 0.0f;

    // No width-based wrapping: split on explicit '\n' only.
    if (wrapWidth <= 0.0f)
    {
        std::vector<StyledRun> lineRuns;
        lineRuns.reserve(16);
        float minLineH = 0.0f;
        size_t lineByteStart = 0;

        auto flushLine = [&](size_t lineByteEnd) {
            const ShapedLine line = shapeLine(lineRuns, curY);
            const float lineH = std::max(line.Height, std::max(1.0f, minLineH));
            curY += (lineBoxPx > 0.0f) ? lineBoxPx : lineH;
            lineRuns.clear();
            minLineH = 0.0f;
            out.LineCount += 1;
            out.Metrics.width = std::max(out.Metrics.width, line.Advance);
            out.LineBreaks.push_back({lineByteStart, lineByteEnd, line.Advance,
                                      line.GlyphStart, line.GlyphEnd, line.Baseline});
            lineByteStart = lineByteEnd + 1;
        };

        size_t globalOffset = 0;
        for (const auto& run : runs)
        {
            if (!run.Font) { globalOffset += run.Text.size(); continue; }
            size_t start = 0;
            while (start <= run.Text.size())
            {
                size_t nl = run.Text.find('\n', start);
                std::string_view seg(run.Text.c_str() + start,
                                     (nl == std::string::npos) ? (run.Text.size() - start) : (nl - start));
                if (!seg.empty())
                {
                    StyledRun r = run;
                    r.Text.assign(seg);
                    lineRuns.push_back(std::move(r));
                }
                minLineH = std::max(minLineH, lineHForRun(&run));
                if (nl == std::string::npos) break;
                flushLine(globalOffset + nl);
                start = nl + 1;
            }
            globalOffset += run.Text.size();
        }
        if (!lineRuns.empty())
            flushLine(globalOffset);
        out.Metrics.height = curY;
        return out;
    }

    // Width-based wrapping: decompose into atoms.
    struct Atom
    {
        const StyledRun* run = nullptr;
        size_t start = 0, end = 0;
        size_t globalStart = 0, globalEnd = 0;
        float advance = 0.0f;
        bool isWhitespace = false;
        bool isNewline = false;
        bool breakAfter = false;
    };

    auto utf8Next = [](std::string_view s, size_t i, uint32_t& outCp) -> size_t {
        if (i >= s.size()) { outCp = 0; return s.size(); }
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { outCp = c; return i + 1; }
        if ((c >> 5) == 0x6 && i + 1 < s.size()) { outCp = ((uint32_t)(c & 0x1F) << 6) | (s[i+1] & 0x3F); return i + 2; }
        if ((c >> 4) == 0xE && i + 2 < s.size()) { outCp = ((uint32_t)(c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); return i + 3; }
        if ((c >> 3) == 0x1E && i + 3 < s.size()) { outCp = ((uint32_t)(c & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); return i + 4; }
        outCp = c; return i + 1;
    };

    auto isWrapWhitespace = [](uint32_t cp) -> bool { return (cp == ' ' || cp == '\t' || cp == '\r'); };

    // Find opportunities in the complete paragraph, not each styled span:
    // styling must not create breaks inside a word. UText keeps offsets in
    // UTF-8 bytes, matching the shaping clusters and public line ranges.
    std::string paragraph;
    for (const auto& run : runs)
        paragraph += run.Text;
    std::vector<bool> breakAfter(paragraph.size() + 1, false);
    bool haveUnicodeBreaks = false;
    if (paragraph.size() <= static_cast<size_t>(std::numeric_limits<int32_t>::max()))
    {
        UErrorCode status = U_ZERO_ERROR;
        std::unique_ptr<UText, decltype(&utext_close)> text(
            utext_openUTF8(nullptr, paragraph.data(), static_cast<int64_t>(paragraph.size()), &status),
            &utext_close);
        std::unique_ptr<UBreakIterator, decltype(&ubrk_close)> breaks(
            ubrk_open(UBRK_LINE, "root", nullptr, 0, &status), &ubrk_close);
        if (U_SUCCESS(status))
            ubrk_setUText(breaks.get(), text.get(), &status);
        if (U_SUCCESS(status))
        {
            haveUnicodeBreaks = true;
            for (int32_t boundary = ubrk_first(breaks.get()); boundary != UBRK_DONE;
                 boundary = ubrk_next(breaks.get()))
                breakAfter[static_cast<size_t>(boundary)] = true;
        }
    }

    std::vector<Atom> atoms;
    atoms.reserve(64);
    size_t globalOffset = 0;
    for (const auto& run : runs)
    {
        if (!run.Font) { globalOffset += run.Text.size(); continue; }
        auto* font = const_cast<FontAtlas*>(run.Font);
        auto measured = font->MeasureText(run.Text, run.PixelSize, run.LetterSpacingPx);
        const auto& caretX = measured.caretXByByte;
        const bool caretOk = !measured.Clusters.empty();
        std::sort(measured.Clusters.begin(), measured.Clusters.end(), [](const auto& left, const auto& right)
        {
            return left.ByteStart < right.ByteStart;
        });
        if (!caretOk || caretX.size() < run.Text.size() + 1)
        {
            size_t start = 0;
            while (start <= run.Text.size())
            {
                size_t nl = run.Text.find('\n', start);
                const size_t len = (nl == std::string::npos) ? (run.Text.size() - start) : (nl - start);
                if (len > 0)
                {
                    std::string_view seg(run.Text.c_str() + start, len);
                    float w = font->MeasureUtf8(seg, run.PixelSize, run.LetterSpacingPx).width;
                    atoms.push_back(Atom{&run, start, start + len, globalOffset + start, globalOffset + start + len, w, false, false});
                }
                if (nl == std::string::npos) break;
                atoms.push_back(Atom{&run, nl, nl + 1, globalOffset + nl, globalOffset + nl + 1, 0.0f, false, true});
                start = nl + 1;
            }
            globalOffset += run.Text.size();
            continue;
        }
        size_t i = 0;
        while (i < run.Text.size())
        {
            uint32_t cp = 0;
            size_t next = utf8Next(run.Text, i, cp);
            if (next <= i) next = i + 1;
            const auto cluster = std::lower_bound(measured.Clusters.begin(), measured.Clusters.end(), i,
                [](const auto& value, size_t byte) { return value.ByteStart < byte; });
            const bool hasCluster = cluster != measured.Clusters.end() && cluster->ByteStart == i;
            if (hasCluster) next = cluster->ByteEnd;
            if (run.Text[i] == '\n')
                atoms.push_back(Atom{&run, i, next, globalOffset + i, globalOffset + next, 0.0f, false, true});
            else
            {
                float adv = hasCluster ? cluster->Right - cluster->Left : 0.0f;
                atoms.push_back(Atom{&run, i, next, globalOffset + i, globalOffset + next, adv,
                                     isWrapWhitespace(cp), false, cp == '-' || (cp != '/' &&
                                     (haveUnicodeBreaks ? breakAfter[globalOffset + next] : isWrapWhitespace(cp)))});
            }
            i = next;
        }
        globalOffset += run.Text.size();
    }

    if (atoms.empty()) return out;

    // Flush a range of atoms as one visual line.
    // emptyBytePos: for empty lines (e.g. consecutive newlines), the global byte offset to use.
    auto flushLineRange = [&](size_t startIdx, size_t endIdx, bool hasAtoms, float fallbackLineH, size_t emptyBytePos = 0) {
        float minLineH = 0.0f;
        if (!hasAtoms)
            minLineH = std::max(1.0f, fallbackLineH);
        else
            for (size_t i = startIdx; i <= endIdx; ++i)
                minLineH = std::max(minLineH, lineHForRun(atoms[i].run));

        size_t gByteStart = emptyBytePos, gByteEnd = emptyBytePos;
        float lineH = 0.0f;
        ShapedLine shaped{};
        float hangWidth = 0.0f;
        if (hasAtoms)
        {
            gByteStart = atoms[startIdx].globalStart;
            gByteEnd = atoms[endIdx].globalEnd;

            // Whitespace the breaker consumed hangs past the wrap point: it is
            // still part of the line's byte range (so caret mapping stays
            // contiguous) but contributes no width, matching CSS.
            for (size_t i = endIdx + 1; i > startIdx; --i)
            {
                if (!atoms[i - 1].isWhitespace)
                    break;
                hangWidth += atoms[i - 1].advance;
            }

            std::vector<StyledRun> lineRuns;
            const StyledRun* curRun = nullptr;
            size_t segStart = 0, segEnd = 0;
            auto pushSeg = [&](const StyledRun* run, size_t s, size_t e) {
                if (!run || s >= e) return;
                StyledRun r = *run;
                r.Text.assign(run->Text.data() + s, e - s);
                lineRuns.push_back(std::move(r));
            };
            for (size_t i = startIdx; i <= endIdx; ++i)
            {
                const auto& atom = atoms[i];
                if (atom.isNewline) continue;
                if (!curRun) { curRun = atom.run; segStart = atom.start; segEnd = atom.end; continue; }
                if (atom.run == curRun && atom.start == segEnd) { segEnd = atom.end; }
                else { pushSeg(curRun, segStart, segEnd); curRun = atom.run; segStart = atom.start; segEnd = atom.end; }
            }
            pushSeg(curRun, segStart, segEnd);
            if (!lineRuns.empty())
            {
                shaped = shapeLine(lineRuns, curY);
                lineH = shaped.Height;
            }
        }
        lineH = std::max(lineH, minLineH);
        curY += (lineBoxPx > 0.0f) ? lineBoxPx : lineH;
        out.LineCount += 1;
        const float lineWidth = std::max(0.0f, shaped.Advance - hangWidth);
        out.Metrics.width = std::max(out.Metrics.width, lineWidth);
        out.LineBreaks.push_back({gByteStart, gByteEnd, lineWidth,
                                  shaped.GlyphStart, shaped.GlyphEnd, shaped.Baseline});
    };

    float lastAtomLineH = 0.0f;
    size_t curStart = 0;
    size_t lastBreak = std::string::npos;
    float curWidth = 0.0f;

    size_t i = 0;
    while (i < atoms.size())
    {
        const Atom& atom = atoms[i];
        float runLineH = lineHForRun(atom.run);
        if (runLineH > 0.0f) lastAtomLineH = runLineH;
        if (atom.isNewline)
        {
            const bool lineHasAtoms = (i > curStart);
            flushLineRange(curStart, lineHasAtoms ? (i - 1) : curStart, lineHasAtoms, lastAtomLineH, atom.globalStart);
            curStart = i + 1; curWidth = 0.0f; lastBreak = std::string::npos; ++i; continue;
        }
        const bool hasContent = (curStart < i);
        const float nextWidth = curWidth + atom.advance;
        // Whitespace never overflows a line: it hangs past the wrap width
        // instead of pushing the following word down a line early.
        if (wrapWidth > 0.0f && !atom.isWhitespace && nextWidth > wrapWidth && hasContent)
        {
            if (lastBreak != std::string::npos && lastBreak >= curStart)
            {
                flushLineRange(curStart, lastBreak, true, lastAtomLineH);
                curStart = lastBreak + 1; curWidth = 0.0f; lastBreak = std::string::npos; i = curStart; continue;
            }
            if (wordBreak == WordBreak::BreakAll || wordBreak == WordBreak::BreakWord)
            {
                flushLineRange(curStart, i - 1, true, lastAtomLineH);
                curStart = i; curWidth = 0.0f; lastBreak = std::string::npos; continue;
            }
        }
        curWidth = nextWidth;
        const bool allowedBreak = wordBreak == WordBreak::KeepAll
            ? (atom.isWhitespace || (atom.end > atom.start && atom.run->Text[atom.end - 1] == '-'))
            : atom.breakAfter;
        if (allowedBreak || wordBreak == WordBreak::BreakAll) lastBreak = i;
        ++i;
    }
    if (curStart < atoms.size())
        flushLineRange(curStart, atoms.size() - 1, true, lastAtomLineH);

    out.Metrics.height = curY;
    return out;
}

} // namespace Text
} // namespace Rendering
} // namespace GameEngine
