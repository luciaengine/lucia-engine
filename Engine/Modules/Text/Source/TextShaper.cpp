#include "FontAtlas_Internal.h"
#include "Logger/Logger.h"
#include "Types/Fnv1a.h"

#include <unicode/ubidi.h>
#include <unicode/locid.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/utf8.h>
#include <unicode/utf16.h>

#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>

namespace GameEngine::Rendering::Text
{
namespace
{
constexpr uint32_t kGlyphMask = 0x00ffffffu;
constexpr unsigned kFaceShift = 24;

#if defined(GE_HAVE_FREETYPE) && defined(GE_HAVE_HARFBUZZ)
struct UnicodeCluster
{
    int32_t Start = 0;
    int32_t End = 0;
    uint32_t ByteStart = 0;
    uint32_t ByteEnd = 0;
    UScriptCode Script = USCRIPT_COMMON;
    unsigned Face = 0;
};

void AppendShapedRun(hb_buffer_t* buffer, unsigned face, bool rtl, uint32_t byteEnd,
                     float letterSpacing, ShapingOutput& out)
{
    unsigned count = 0;
    const auto* info = hb_buffer_get_glyph_infos(buffer, &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
    std::vector<uint32_t> boundaries{byteEnd};
    for (unsigned index = 0; index < count; ++index)
        boundaries.push_back(info[index].cluster);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    for (unsigned index = 0; index < count; ++index)
    {
        const auto end = std::upper_bound(boundaries.begin(), boundaries.end(), info[index].cluster);
        out.glyphIds.push_back((face << kFaceShift) | info[index].codepoint);
        const bool clusterEnd = index + 1 == count || info[index + 1].cluster != info[index].cluster;
        out.baseXAdv.push_back(positions[index].x_advance / 64.0f);
        out.xAdv.push_back(positions[index].x_advance / 64.0f + (clusterEnd ? letterSpacing : 0.0f));
        out.xOff.push_back(positions[index].x_offset / 64.0f);
        out.yOff.push_back(positions[index].y_offset / 64.0f);
        out.clusterStart.push_back(info[index].cluster);
        out.clusterEnd.push_back(end == boundaries.end() ? byteEnd : *end);
        out.clusterRTL.push_back(rtl);
    }
}

bool CoversCluster(FT_Face face, std::string_view text)
{
    if (!face)
        return false;
    int32_t offset = 0;
    const int32_t length = static_cast<int32_t>(text.size());
    while (offset < length)
    {
        UChar32 codepoint;
        U8_NEXT(text.data(), offset, length, codepoint);
        if (codepoint < 0)
            codepoint = 0xfffd;
        if (!u_hasBinaryProperty(codepoint, UCHAR_DEFAULT_IGNORABLE_CODE_POINT) &&
            !u_iscntrl(codepoint) && FT_Get_Char_Index(face, codepoint) == 0)
            return false;
    }
    return true;
}

// Reads the file through the path object, never a narrow string, so a face under a directory
// name outside the system code page still opens.
bool OpenFaceFile(FT_Library library, const std::filesystem::path& path, FreeTypeFace& face)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), {}};
    return !bytes.empty() && bytes.size() <= static_cast<size_t>(std::numeric_limits<FT_Long>::max()) &&
           face.NewMemoryFace(library, bytes.data(), bytes.size());
}
#endif
}

struct TextShaper::FallbackFace
{
    std::filesystem::path Path;
    bool Attempted = false;
#if defined(GE_HAVE_FREETYPE)
    FreeTypeFace Face;
#endif
#if defined(GE_HAVE_HARFBUZZ)
    HBFont Font;
#endif
};

TextShaper::TextShaper() = default;
TextShaper::~TextShaper() = default;

bool TextShaper::SetFallbackFonts(std::vector<std::filesystem::path> fonts)
{
    if (fonts.size() > 255 || std::any_of(fonts.begin(), fonts.end(), [](const auto& path) { return path.empty(); }))
        return false;
    m_FallbackFaces.clear();
    for (auto& path : fonts)
    {
        auto face = std::make_unique<FallbackFace>();
        face->Path = std::move(path);
        m_FallbackFaces.push_back(std::move(face));
    }
    ++m_Revision;
    return true;
}

uint64_t TextShaper::GetFallbackIdentity() const
{
    if (m_FallbackFaces.empty()) return 0;
    uint64_t hash = Hashing::kFnv1a64OffsetBasis;
    for (const auto& face : m_FallbackFaces)
    {
        // UTF-8 on every platform: a narrow conversion throws for names outside the code page.
        const auto path = face->Path.lexically_normal().generic_u8string();
        hash = Hashing::Fnv1a64Value(hash, path.size());
        hash = Hashing::Fnv1a64(path.data(), path.size(), hash);
    }
    return hash;
}

bool TextShaper::SetLanguage(std::string_view language)
{
    if (language.empty() || language.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return false;
    UErrorCode error = U_ZERO_ERROR;
    const auto locale = icu::Locale::forLanguageTag(
        icu::StringPiece(language.data(), static_cast<int32_t>(language.size())), error);
    const auto canonical = locale.toLanguageTag<std::string>(error);
    if (U_FAILURE(error)) return false;
    if (m_Language != canonical)
    {
        m_Language = canonical;
        ++m_Revision;
    }
    return true;
}

#if defined(GE_HAVE_FREETYPE)
FT_Face TextShaper::ResolveGlyphFace(FT_Face primary, uint32_t& glyphId) const
{
    const unsigned slot = glyphId >> kFaceShift;
    glyphId &= kGlyphMask;
    if (slot == 0)
        return primary;
    if (slot > m_FallbackFaces.size())
        return nullptr;
    return m_FallbackFaces[slot - 1]->Face.Face();
}
#endif

#if defined(GE_HAVE_FREETYPE) && defined(GE_HAVE_HARFBUZZ)
void TextShaper::Shape(FreeTypeLib& library, FreeTypeFace& primary, HBFont& primaryFont,
                       std::string_view utf8, unsigned atlasSize,
                       ShapingOutput& out, float letterSpacingAtlas, size_t begin, size_t end)
{
    if (utf8.empty() || utf8.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return;

    begin = std::min(begin, utf8.size());
    end = std::min(end, utf8.size());
    if (begin >= end) return;
    // The common single-script path does not need bidi or grapheme segmentation.
    const bool simpleAscii = std::all_of(utf8.begin(), utf8.end(), [](unsigned char character)
    {
        return character >= 0x20 && character < 0x7f;
    });
    if (simpleAscii && CoversCluster(primary.Face(), utf8))
    {
        hb_buffer_t* buffer = Detail::GetThreadLocalHBBuffer();
        hb_buffer_add_utf8(buffer, utf8.data(), static_cast<int>(utf8.size()), static_cast<unsigned>(begin), static_cast<int>(end-begin));
        hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
        hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
        hb_buffer_set_language(buffer, hb_language_from_string(m_Language.c_str(), -1));
        hb_feature_t features[2];
        const unsigned count = Detail::LetterSpacingFeatures(letterSpacingAtlas, features);
        hb_shape(primaryFont.Get(), buffer, count ? features : nullptr, count);
        AppendShapedRun(buffer, 0, false, static_cast<uint32_t>(end), letterSpacingAtlas, out);
        return;
    }

    // Preserve original UTF-8 byte boundaries through ICU's UTF-16 indexing.
    std::vector<UChar> text;
    std::vector<uint32_t> bytes;
    int32_t offset = 0;
    const int32_t length = static_cast<int32_t>(utf8.size());
    while (offset < length)
    {
        const uint32_t start = static_cast<uint32_t>(offset);
        UChar32 codepoint;
        U8_NEXT(utf8.data(), offset, length, codepoint);
        if (codepoint < 0)
            codepoint = 0xfffd;
        bytes.push_back(start);
        if (codepoint > 0xffff)
        {
            text.push_back(U16_LEAD(codepoint));
            bytes.push_back(start);
            text.push_back(U16_TRAIL(codepoint));
        }
        else
            text.push_back(static_cast<UChar>(codepoint));
    }
    bytes.push_back(static_cast<uint32_t>(utf8.size()));
    const int32_t textLength = static_cast<int32_t>(text.size());
    UErrorCode error = U_ZERO_ERROR;
    std::unique_ptr<UBiDi, decltype(&ubidi_close)> bidi(ubidi_open(), ubidi_close);
    std::unique_ptr<UBreakIterator, decltype(&ubrk_close)> breaks(
        ubrk_open(UBRK_CHARACTER, "root", text.data(), textLength, &error), ubrk_close);
    if (!bidi || U_FAILURE(error) || !breaks)
        return;
    ubidi_setPara(bidi.get(), text.data(), textLength, UBIDI_DEFAULT_LTR, nullptr, &error);
    if (U_FAILURE(error))
        return;

    std::vector<UnicodeCluster> clusters;
    UScriptCode previousScript = USCRIPT_COMMON;
    int32_t start = ubrk_first(breaks.get());
    for (int32_t breakEnd = ubrk_next(breaks.get()); breakEnd != UBRK_DONE;
         start = breakEnd, breakEnd = ubrk_next(breaks.get()))
    {
        UnicodeCluster cluster{start, breakEnd, bytes[start], bytes[breakEnd], previousScript, 0};
        for (int32_t index = start; index < breakEnd;)
        {
            UChar32 codepoint;
            U16_NEXT(text.data(), index, breakEnd, codepoint);
            UErrorCode scriptError = U_ZERO_ERROR;
            const auto script = uscript_getScript(codepoint, &scriptError);
            if (U_SUCCESS(scriptError) && script != USCRIPT_COMMON && script != USCRIPT_INHERITED)
            {
                cluster.Script = script;
                previousScript = script;
                break;
            }
        }
        const auto clusterText = utf8.substr(cluster.ByteStart, cluster.ByteEnd - cluster.ByteStart);
        if (!CoversCluster(primary.Face(), clusterText))
        {
            for (size_t index = 0; index < m_FallbackFaces.size(); ++index)
            {
                auto& fallback = *m_FallbackFaces[index];
                if (!fallback.Attempted)
                {
                    fallback.Attempted = true;
                    if (OpenFaceFile(library.Lib(), fallback.Path, fallback.Face) &&
                        fallback.Face.Face()->num_glyphs <= static_cast<FT_Long>(kGlyphMask))
                    {
                        fallback.Face.SetPixelSizes(atlasSize);
                        fallback.Font.CreateFromFT(fallback.Face.Face());
                        fallback.Font.SetLoadFlags(FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP |
                                                   FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT);
                    }
                }
                if (fallback.Font.Get() && CoversCluster(fallback.Face.Face(), clusterText))
                {
                    cluster.Face = static_cast<unsigned>(index + 1);
                    break;
                }
            }
            if (cluster.Face == 0)
                ReportMissingGlyphs(primary.Face(), cluster.Script);
        }
        clusters.push_back(cluster);
    }
    // Leading punctuation adopts the first script, while marks stay with their base.
    UScriptCode nextScript = USCRIPT_COMMON;
    for (auto it = clusters.rbegin(); it != clusters.rend(); ++it)
    {
        if (it->Script == USCRIPT_COMMON)
            it->Script = nextScript;
        else
            nextScript = it->Script;
    }

    const int32_t runCount = ubidi_countRuns(bidi.get(), &error);
    if (U_FAILURE(error))
        return;
    for (int32_t run = 0; run < runCount; ++run)
    {
        int32_t runStart, runLength;
        const bool rtl = ubidi_getVisualRun(bidi.get(), run, &runStart, &runLength) == UBIDI_RTL;
        struct Segment { size_t First; size_t End; };
        std::vector<Segment> segments;
        for (size_t index = 0; index < clusters.size();)
        {
            if (clusters[index].Start < runStart || clusters[index].Start >= runStart + runLength ||
                clusters[index].ByteStart < begin || clusters[index].ByteStart >= end)
            {
                ++index;
                continue;
            }
            const size_t first = index++;
            while (index < clusters.size() && clusters[index].Start < runStart + runLength &&
                   clusters[index].ByteStart < end &&
                   clusters[index].Script == clusters[first].Script &&
                   clusters[index].Face == clusters[first].Face)
                ++index;
            segments.push_back({first, index});
        }
        if (rtl)
            std::reverse(segments.begin(), segments.end());
        for (const auto& segment : segments)
        {
            const auto& first = clusters[segment.First];
            const uint32_t byteEnd = clusters[segment.End - 1].ByteEnd;
            hb_font_t* font = first.Face == 0 ? primaryFont.Get() : m_FallbackFaces[first.Face - 1]->Font.Get();
            if (first.Face != 0)
            {
                m_FallbackFaces[first.Face - 1]->Face.SetPixelSizes(atlasSize);
                hb_ft_font_changed(font);
                hb_font_set_scale(font, atlasSize * 64, atlasSize * 64);
                hb_font_set_ppem(font, atlasSize, atlasSize);
            }
            hb_buffer_t* buffer = Detail::GetThreadLocalHBBuffer();
            hb_buffer_add_utf8(buffer, utf8.data(), length, first.ByteStart, byteEnd - first.ByteStart);
            hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
            hb_buffer_set_script(buffer, hb_script_from_string(uscript_getShortName(first.Script), -1));
            hb_buffer_set_language(buffer, hb_language_from_string(m_Language.c_str(), -1));
            hb_feature_t features[2];
            const unsigned featureCount = Detail::LetterSpacingFeatures(letterSpacingAtlas, features);
            hb_shape(font, buffer, featureCount ? features : nullptr, featureCount);
            AppendShapedRun(buffer, first.Face, rtl, byteEnd, letterSpacingAtlas, out);
        }
    }
}

void TextShaper::ReportMissingGlyphs(FT_Face primary, int32_t script)
{
    const std::string family = primary && primary->family_name ? primary->family_name : "unnamed";
    if (!m_ReportedMissingGlyphs.emplace(family, script).second)
        return;
    const char* scriptName = uscript_getName(static_cast<UScriptCode>(script));
    if (!scriptName)
        scriptName = "unknown script";
    Logger::Log::Warning(
        "[Text] Font '{}' and its {} fallback font(s) do not cover some {} characters; missing glyphs will not render. "
        "Add a fallback font that covers {} to the font's fallback list.",
        family, m_FallbackFaces.size(), scriptName, scriptName);
}
#endif
#if defined(GE_HAVE_FREETYPE)
bool TextShaper::HasColorFallback() const
{
    return std::any_of(m_FallbackFaces.begin(), m_FallbackFaces.end(), [](const auto& face)
    { return face->Face.IsValid() && FT_HAS_COLOR(face->Face.Face()); });
}
#endif
}
