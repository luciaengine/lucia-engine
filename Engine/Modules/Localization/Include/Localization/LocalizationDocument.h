#pragma once
#include "Localization/LocalizationCatalog.h"
#include <string_view>
#include <vector>

namespace GameEngine::Localization
{
struct TranslationEntry
{
    std::string Key;
    std::string Context;
    std::string Notes;
    std::map<std::string, std::string> Translations;
};

// Editable catalog on the editor thread. Mutations validate a complete candidate
// before adoption; runtime catalogs remain independent snapshots.
class LocalizationDocument
{
public:
    bool Load(std::istream& input, std::string& error);
    bool Write(std::ostream& output, std::string& error) const;
    bool Update(TranslationEntry entry, std::string& error);
    bool AddLocale(const std::string& locale, std::string& error);
    const std::map<StringId, TranslationEntry>& Entries() const { return m_Entries; }
    const std::vector<std::string>& Locales() const { return m_Locales; }
    // The entries with an English source whose `locale` translation is absent, empty, or (for a
    // translation locale) has a plural without a case for one of its plural categories; sorted by id.
    std::vector<StringId> Missing(const std::string& locale) const;
    // The entries whose key contains `filter` (case-insensitive, ASCII letters), sorted by key;
    // with `missingOnly`, only those Missing(locale) lists. An empty filter matches every key.
    std::vector<StringId> FindKeys(std::string_view filter, const std::string& locale, bool missingOnly) const;
    std::shared_ptr<LocalizationCatalog> CreatePreview(const std::string& locale, std::string& error) const;
    bool ExportPo(std::ostream& output, const std::string& locale, std::string& error) const;
    bool ImportPo(std::istream& input, const std::string& locale, std::string& error);
private:
    bool Validate(std::string& error) const;
    std::map<StringId, TranslationEntry> m_Entries;
    std::vector<std::string> m_Locales = {"en"};
};
// Preview-only transformation of formatted text: no mutation of ICU patterns or
// shipping translations. Accents, padding and boundary markers expose clipping.
std::string Pseudolocalize(std::string_view formattedText);
}
