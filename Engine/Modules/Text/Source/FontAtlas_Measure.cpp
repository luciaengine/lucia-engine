#include "FontAtlas_Internal.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::Rendering::Text
{

#if defined(GE_HAVE_FREETYPE)
namespace
{
// The three design-unit parts of a CSS `normal` line box.
struct NormalLineUnits
{
    float Ascent = 0.0f;
    float Descent = 0.0f;
    float LineGap = 0.0f;
};

// Which of the face's three competing metric sets a `normal` line box is built
// from. Blink asks the platform font API, which on Windows is DirectWrite:
// DWRITE_FONT_METRICS reports the OS/2 usWin pair plus only whatever hhea
// leading those do not already account for, unless the face opts into its
// typographic metrics via fsSelection bit 7 (USE_TYPO_METRICS).
//
// The distinction is invisible on faces whose three sets agree (Roboto,
// Roboto Mono) and decides the whole line box on faces where they do not:
// Cascadia Code sets the bit and would otherwise be 14% too tall, and Consolas
// and Calibri keep their totals but move the baseline by 2px at 16px because
// hhea splits the same height differently.
static NormalLineUnits NormalLineUnitsForFace(FT_Face face)
{
    NormalLineUnits u{(float)face->ascender, (float)(-face->descender),
                      (float)(face->height - (face->ascender - face->descender))};

    const auto* hhea = (const TT_HoriHeader*)FT_Get_Sfnt_Table(face, FT_SFNT_HHEA);
    const auto* os2 = (const TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    // 0xFFFF is FreeType's "this face has no OS/2 table"; a non-SFNT face has
    // neither table. Both leave hhea as the only metric set there is.
    if (!hhea || !os2 || os2->version == 0xFFFFu)
        return u;

    constexpr FT_UShort kFsSelectionUseTypoMetrics = 1u << 7;
    if (os2->fsSelection & kFsSelectionUseTypoMetrics)
    {
        u.Ascent = (float)os2->sTypoAscender;
        u.Descent = (float)(-os2->sTypoDescender);
        u.LineGap = (float)os2->sTypoLineGap;
        return u;
    }

    const float winAscent = (float)os2->usWinAscent;
    const float winDescent = (float)os2->usWinDescent;
    const float hheaExtent = (float)(hhea->Ascender - hhea->Descender);
    u.Ascent = winAscent;
    u.Descent = winDescent;
    // The usWin pair usually already covers part of hhea's gap; only the
    // remainder survives as external leading, and never a negative amount.
    u.LineGap = std::max(0.0f, (float)hhea->Line_Gap - ((winAscent + winDescent) - hheaExtent));
    return u;
}

// A CSS `normal` line box, in the pixel space the face is rasterized in.
//
// Design units scaled to the render size, not FreeType's size->metrics: those
// are rounded to whole pixels at the active ppem, so reading them bakes the
// atlas ppem's rounding into every render size and makes the reported height
// move when the atlas size changes.
//
// Blink rounds ascent, descent and line gap to whole pixels independently and
// sums them (SimpleFontData::PlatformInit), then places the baseline at
// floor(ascent + halfLeading) (FontHeight::AddLeading). Both roundings happen
// at the size the font was instantiated at, so renderPixelSize being a DEVICE
// pixel size is what keeps this matching Chrome on a scaled display: Chrome
// folds the device scale factor into the used font size, and its line box in
// CSS px is correspondingly fractional there.
static LineMetrics ExactLineMetricsWithFace(FT_Face face, float renderPixelSize)
{
    const float upem = (face->units_per_EM > 0) ? (float)face->units_per_EM : 1000.0f;
    const float unitScale = renderPixelSize / upem;
    const NormalLineUnits u = NormalLineUnitsForFace(face);

    const float ascPx = std::round(u.Ascent * unitScale);
    const float descPx = std::round(u.Descent * unitScale);
    const float gapPx = std::round(u.LineGap * unitScale);

    LineMetrics lm{};
    lm.height = ascPx + descPx + gapPx;
    lm.ascender = std::floor(ascPx + gapPx * 0.5f);
    lm.descender = lm.height - lm.ascender;
    return lm;
}

} // namespace
#endif // GE_HAVE_FREETYPE

bool FontAtlas::GetTypographicHeights(unsigned pixelSize,
                                      float& outCapHeight, bool& outCapValid,
                                      float& outXHeight, bool& outXValid,
                                      float& outWinAscent, float& outWinDescent)
{
#if defined(GE_HAVE_FREETYPE)
    outCapHeight = 0.0f;
    outXHeight = 0.0f;
    outWinAscent = 0.0f;
    outWinDescent = 0.0f;
    outCapValid = false;
    outXValid = false;
    FT_Face face = m_FtFace.Face();
    if (!face)
        return false;
    // Temporarily set to requested render size; restore to atlas size (or pixelSize if 0) afterwards.
    unsigned prevPx = (m_AtlasPixelSize > 0) ? m_AtlasPixelSize : pixelSize;
    m_FtFace.SetPixelSizes(pixelSize);
#if defined(GE_HAVE_HARFBUZZ)
    if (m_HbFont.Get())
    {
        hb_ft_font_changed(m_HbFont.Get());
    }
#endif
    double upm = (face->units_per_EM ? (double)face->units_per_EM : 2048.0);
    double yppem = (face->size && face->size->metrics.y_ppem ? (double)face->size->metrics.y_ppem : (double)pixelSize);
    double scale = (upm > 0.0) ? (yppem / upm) : 1.0;
    TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2)
    {
        outWinAscent = (float)(os2->usWinAscent * scale);
        outWinDescent = (float)(os2->usWinDescent * scale);
        if (os2->version >= 2)
        {
            if (os2->sCapHeight != 0)
            {
                outCapHeight = (float)(os2->sCapHeight * scale);
                outCapValid = true;
            }
            if (os2->sxHeight != 0)
            {
                outXHeight = (float)(os2->sxHeight * scale);
                outXValid = true;
            }
        }
    }
    else
    {
        // Fallback to FT size metrics (hhea asc/desc) if OS/2 absent and size metrics available.
        if (face->size)
        {
            const FT_Size_Metrics& mt = face->size->metrics;
            outWinAscent = (float)(mt.ascender / 64.0);
            outWinDescent = (float)(-mt.descender / 64.0);
        }
    }
    // Restore atlas pixel size
    m_FtFace.SetPixelSizes(prevPx);
#if defined(GE_HAVE_HARFBUZZ)
    if (m_HbFont.Get())
    {
        hb_ft_font_changed(m_HbFont.Get());
    }
#endif
    return true;
#else
    (void)pixelSize;
    (void)outCapHeight;
    (void)outCapValid;
    (void)outXHeight;
    (void)outXValid;
    (void)outWinAscent;
    (void)outWinDescent;
    return false;
#endif
}

bool FontAtlas::RendersAllGlyphs(std::string_view utf8)
{
    if (utf8.empty()) return true;
#if defined(GE_HAVE_FREETYPE)
    if (!m_FtFace.IsValid()) return false;
#else
    return false;
#endif
    constexpr uint32_t kGlyphIndexMask = 0x00FFFFFFu;
    constexpr float kProbePixelSize = 16.0f;
    ShapingOutput shaped;
    ShapeGlyphs(utf8, kProbePixelSize, false, shaped, 0.0f);
    for (size_t index = 0; index < shaped.glyphIds.size(); ++index)
    {
        const uint32_t start = index < shaped.clusterStart.size() ? shaped.clusterStart[index] : 0;
        if (start < utf8.size() && static_cast<unsigned char>(utf8[start]) < 0x20u) continue;
        if ((shaped.glyphIds[index] & kGlyphIndexMask) == 0) return false;
    }
    return true;
}

bool FontAtlas::HasGlyph(uint32_t codepoint) const
{
#if !defined(GE_HAVE_FREETYPE)
    (void)codepoint;
    return false;
#else
    if (!m_FtFace.IsValid())
        return false;
    FT_UInt gid = FT_Get_Char_Index(m_FtFace.Face(), (FT_ULong)codepoint);
    return gid != 0;
#endif
}

TextMetrics FontAtlas::MeasureUtf8(std::string_view utf8, float pixelSize,
                                   float letterSpacingPx)
{
    TextMetrics metrics{};
#if defined(GE_HAVE_FREETYPE)
    if (!m_FtFace.IsValid())
        return metrics;
    const unsigned rasterSize = RasterPixelSize(pixelSize);
    const unsigned atlasSize = m_AtlasPixelSize > 0 ? m_AtlasPixelSize : rasterSize;
    const float scale = std::max(1.0f, pixelSize) / atlasSize;
    const auto key = ShapedWidthKey(utf8, atlasSize, letterSpacingPx != 0.0f);
    float width = 0.0f;
    uint32_t clusterCount = 0;
    if (!m_ShapedWidthCache || !ShapedWidthLookup(*m_ShapedWidthCache, key, width, clusterCount))
    {
        ShapingOutput shaped;
        ShapeGlyphs(utf8, pixelSize, false, shaped, letterSpacingPx);
        for (size_t index = 0; index < shaped.xAdv.size(); ++index)
        {
            width += shaped.baseXAdv.empty() ? shaped.xAdv[index] : shaped.baseXAdv[index];
            if (shaped.clusterStart.empty() || index + 1 == shaped.xAdv.size() ||
                shaped.clusterStart[index + 1] != shaped.clusterStart[index])
                ++clusterCount;
        }
        if (shaped.baseXAdv.empty()) width -= clusterCount * letterSpacingPx / scale;
        if (m_ShapedWidthCache)
            ShapedWidthInsert(*m_ShapedWidthCache, key, width, clusterCount);
    }
    const auto line = GetLineMetrics(pixelSize);
    metrics.width = width * scale + clusterCount * letterSpacingPx;
    metrics.ascender = line.ascender;
    metrics.descender = line.descender;
    metrics.height = line.height;
    metrics.baseline = line.ascender;
#endif
    return metrics;
}

namespace
{
std::vector<size_t> VisualCaretStops(const FontAtlas::MeasureResult& measurement)
{
    std::vector<size_t> stops;
    if (measurement.caretXByByte.empty()) return stops;
    stops.push_back(0);
    stops.push_back(measurement.caretXByByte.size() - 1);
    for (const auto& cluster : measurement.Clusters)
    {
        if (cluster.ByteStart < measurement.caretXByByte.size()) stops.push_back(cluster.ByteStart);
        if (cluster.ByteEnd < measurement.caretXByByte.size()) stops.push_back(cluster.ByteEnd);
    }
    std::sort(stops.begin(), stops.end());
    stops.erase(std::unique(stops.begin(), stops.end()), stops.end());
    std::stable_sort(stops.begin(), stops.end(), [&](size_t left, size_t right)
    {
        return measurement.caretXByByte[left] < measurement.caretXByByte[right];
    });
    return stops;
}
}

size_t FontAtlas::MeasureResult::NextVisualCaret(size_t current, bool right) const
{
    const auto stops = VisualCaretStops(*this);
    if (stops.empty()) return 0;
    const auto found = std::find(stops.begin(), stops.end(), current);
    if (found == stops.end())
        return CaretAtX(caretXByByte[std::min(current, caretXByByte.size() - 1)]);
    if (right) return std::next(found) == stops.end() ? *found : *std::next(found);
    return found == stops.begin() ? *found : *std::prev(found);
}

size_t FontAtlas::MeasureResult::CaretAtX(float x) const
{
    const auto stops = VisualCaretStops(*this);
    if (stops.empty()) return 0;
    return *std::min_element(stops.begin(), stops.end(), [&](size_t left, size_t right)
    {
        return std::abs(caretXByByte[left] - x) < std::abs(caretXByByte[right] - x);
    });
}

std::vector<std::pair<float, float>> FontAtlas::MeasureResult::SelectionRanges(size_t start, size_t end) const
{
    if (start > end) std::swap(start, end);
    std::vector<std::pair<float, float>> ranges;
    if (start == end) return ranges;
    for (const auto& cluster : Clusters)
        if (cluster.ByteStart < end && cluster.ByteEnd > start && cluster.Right > cluster.Left)
            ranges.emplace_back(cluster.Left, cluster.Right);
    std::sort(ranges.begin(), ranges.end());
    size_t count = 0;
    for (const auto& range : ranges)
    {
        if (count && range.first <= ranges[count - 1].second + 0.001f)
            ranges[count - 1].second = std::max(ranges[count - 1].second, range.second);
        else
            ranges[count++] = range;
    }
    ranges.resize(count);
    return ranges;
}

FontAtlas::MeasureResult FontAtlas::MeasureText(std::string_view utf8, float pixelSize,
                                                float letterSpacingPx)
{
    MeasureResult r{};
    r.metrics = MeasureUtf8(utf8, pixelSize, letterSpacingPx);
    BuildCaretMapUtf8(utf8, pixelSize, r.caretXByByte, letterSpacingPx, &r.Clusters);
    return r;
}

LineMetrics FontAtlas::GetLineMetrics(float pixelSize) const
{
#if !defined(GE_HAVE_FREETYPE)
    (void)pixelSize;
    return {};
#else
    if (!m_FtFace.IsValid())
        return {};
    return ExactLineMetricsWithFace(m_FtFace.Face(), pixelSize);
#endif
}

LineMetrics FontAtlas::GetFontLineMetrics(float pixelSize) const
{
    return GetLineMetrics(pixelSize);
}

bool FontAtlas::BuildCaretMapUtf8(std::string_view utf8, float pixelSize,
                                  std::vector<float>& outXByByte, float letterSpacingPx,
                                  std::vector<TextCluster>* outClusters)
{
    if (outClusters) outClusters->clear();
    outXByByte.assign(utf8.size() + 1, 0.0f);
#if defined(GE_HAVE_FREETYPE)
    if (!m_FtFace.IsValid())
        return false;
    ShapingOutput shaped;
    const float scale = ShapeGlyphs(utf8, pixelSize, false, shaped, letterSpacingPx);
    struct CaretCluster { uint32_t Start; uint32_t End; float StartX; float EndX; };
    std::vector<CaretCluster> clusters;
    float pen = 0.0f;
    for (size_t index = 0; index < shaped.clusterStart.size();)
    {
        const size_t first = index;
        const float left = pen;
        do
        {
            pen += shaped.xAdv[index++] * scale;
        } while (index < shaped.clusterStart.size() &&
                 shaped.clusterStart[index] == shaped.clusterStart[first]);
        const bool rtl = shaped.clusterRTL[first];
        if (outClusters)
            outClusters->push_back({shaped.clusterStart[first], shaped.clusterEnd[first], std::min(left, pen), std::max(left, pen)});
        clusters.push_back({shaped.clusterStart[first], shaped.clusterEnd[first],
                            rtl ? pen : left, rtl ? left : pen});
    }
    // At a bidi boundary prefer the following logical cluster's leading edge.
    for (const auto& cluster : clusters)
        if (cluster.End < outXByByte.size())
            outXByByte[cluster.End] = cluster.EndX;
    for (const auto& cluster : clusters)
        for (size_t byte = cluster.Start; byte < cluster.End && byte < outXByByte.size(); ++byte)
            outXByByte[byte] = cluster.StartX;
    return true;
#else
    (void)pixelSize;
    (void)letterSpacingPx;
    return false;
#endif
}

} // namespace GameEngine::Rendering::Text
