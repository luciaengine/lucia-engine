#pragma once
#include "Rendering/Text/FTUtils.h"
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <string_view>

namespace GameEngine::Rendering::Text
{
struct ShapingOutput;

// Unicode segmentation and font selection. Glyph atlas storage is owned by FontAtlas.
class TextShaper
{
public:
    TextShaper();
    ~TextShaper();
    bool SetFallbackFonts(std::vector<std::filesystem::path> fonts);
    bool SetLanguage(std::string_view language);
    uint64_t GetFallbackIdentity() const;
    uint64_t GetRevision() const { return m_Revision; }
#if defined(GE_HAVE_FREETYPE)
    FT_Face ResolveGlyphFace(FT_Face primary, uint32_t& glyphId) const;
    bool HasColorFallback() const;
#endif
#if defined(GE_HAVE_FREETYPE) && defined(GE_HAVE_HARFBUZZ)
    void Shape(FreeTypeLib& library, FreeTypeFace& primary, HBFont& primaryFont,
               std::string_view utf8, unsigned atlasSize, ShapingOutput& out,
               float letterSpacing, size_t begin, size_t end);
#endif
private:
#if defined(GE_HAVE_FREETYPE) && defined(GE_HAVE_HARFBUZZ)
    void ReportMissingGlyphs(FT_Face primary, int32_t script);
#endif
    struct FallbackFace;
    std::vector<std::unique_ptr<FallbackFace>> m_FallbackFaces;
    // BCP-47 "und" = undetermined: no language-specific shaping preference.
    std::string m_Language = "und";
    uint64_t m_Revision = 0;
    std::set<std::pair<std::string, int32_t>> m_ReportedMissingGlyphs;
};
}
