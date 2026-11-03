#include "Localization/LocalizationDocument.h"
#include "Localization/MessageCheck.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace GameEngine::Localization
{
namespace
{
bool IsMissing(const TranslationEntry& entry, const std::string& locale)
{
    if (!entry.Translations.contains("en")) return false;
    const auto text = entry.Translations.find(locale);
    if (text == entry.Translations.end() || text->second.empty()) return true;
    // The English source is never missing; plural categories are checked on translations.
    return locale != "en" && !MissingPluralCategories(text->second, locale).empty();
}
}

bool LocalizationDocument::Load(std::istream& input, std::string& error)
{
    try
    {
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        LocalizationCatalog catalog;
        std::istringstream validation(bytes);
        if (!catalog.Load(validation, error)) return false;
        const auto json = nlohmann::json::parse(bytes);
        LocalizationDocument candidate;
        candidate.m_Locales.clear();
        for (const auto& [locale, entries] : json.items())
        {
            if (locale == "$metadata") continue;
            if (!catalog.SetLocale(locale, error)) return false;
            const auto normalized = catalog.GetLocale();
            candidate.m_Locales.push_back(normalized);
            for (const auto& [key, value] : entries.items())
            {
                auto& entry = candidate.m_Entries[HashStringId(key)];
                entry.Key = key;
                entry.Translations[normalized] = value.get<std::string>();
            }
        }
        if (json.contains("$metadata"))
            for (const auto& [key, metadata] : json.at("$metadata").items())
            {
                const auto found = candidate.m_Entries.find(HashStringId(key));
                if (found == candidate.m_Entries.end()) throw std::runtime_error("Metadata references unknown key: " + key);
                found->second.Context = metadata.value("context", std::string{});
                found->second.Notes = metadata.value("notes", std::string{});
            }
        *this = std::move(candidate);
        error.clear();
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool LocalizationDocument::Write(std::ostream& output, std::string& error) const
{
    try
    {
        auto json = nlohmann::json::object();
        for (const auto& locale : m_Locales) json[locale] = nlohmann::json::object();
        for (const auto& [id, entry] : m_Entries)
        {
            for (const auto& [locale, text] : entry.Translations) json[locale][entry.Key] = text;
            if (!entry.Context.empty()) json["$metadata"][entry.Key]["context"] = entry.Context;
            if (!entry.Notes.empty()) json["$metadata"][entry.Key]["notes"] = entry.Notes;
        }
        output << json.dump(2) << '\n';
        if (!output) throw std::runtime_error("Could not write catalog.");
        error.clear(); return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool LocalizationDocument::Validate(std::string& error) const
{
    std::ostringstream bytes;
    if (!Write(bytes, error)) return false;
    std::istringstream input(bytes.str());
    LocalizationCatalog catalog;
    return catalog.Load(input, error);
}
bool LocalizationDocument::Update(TranslationEntry entry, std::string& error)
{
    const auto id = HashStringId(entry.Key);
    if (const auto found = m_Entries.find(id); found != m_Entries.end() && found->second.Key != entry.Key)
    { error = "Translation key hash collision."; return false; }
    if (!entry.Translations.contains("en")) { error = "Provide English source text."; return false; }
    for (const auto& [locale, text] : entry.Translations)
        if (std::find(m_Locales.begin(), m_Locales.end(), locale) == m_Locales.end())
        { error = "Add the locale before editing its translations: " + locale; return false; }
    auto candidate = *this;
    candidate.m_Entries[id] = std::move(entry);
    if (!candidate.Validate(error)) return false;
    *this = std::move(candidate); return true;
}
bool LocalizationDocument::AddLocale(const std::string& locale, std::string& error)
{
    LocalizationCatalog catalog;
    if (!catalog.SetLocale(locale, error)) return false;
    const auto normalized = catalog.GetLocale();
    if (std::find(m_Locales.begin(), m_Locales.end(), normalized) == m_Locales.end())
        m_Locales.push_back(normalized);
    return true;
}
std::vector<StringId> LocalizationDocument::Missing(const std::string& locale) const
{
    std::vector<StringId> result;
    for (const auto& [id, entry] : m_Entries)
        if (IsMissing(entry, locale)) result.push_back(id);
    return result;
}
std::vector<StringId> LocalizationDocument::FindKeys(std::string_view filter, const std::string& locale, bool missingOnly) const
{
    const auto lower = [](std::string_view text)
    {
        std::string result(text);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    };
    const auto needle = lower(filter);
    std::vector<std::pair<std::string, StringId>> found;
    for (const auto& [id, entry] : m_Entries)
    {
        if (missingOnly && !IsMissing(entry, locale)) continue;
        auto key = lower(entry.Key);
        if (key.find(needle) == std::string::npos) continue;
        found.emplace_back(std::move(key), id);
    }
    // Case-insensitive order, ties broken by the exact key so the order is total.
    std::sort(found.begin(), found.end(), [this](const auto& left, const auto& right)
    {
        if (left.first != right.first) return left.first < right.first;
        return m_Entries.at(left.second).Key < m_Entries.at(right.second).Key;
    });
    std::vector<StringId> result;
    result.reserve(found.size());
    for (const auto& item : found) result.push_back(item.second);
    return result;
}
std::shared_ptr<LocalizationCatalog> LocalizationDocument::CreatePreview(const std::string& locale, std::string& error) const
{
    std::ostringstream bytes;
    if (!Write(bytes, error)) return {};
    std::istringstream input(bytes.str());
    auto catalog = std::make_shared<LocalizationCatalog>();
    if (!catalog->Load(input, error) || !catalog->SetLocale(locale, error)) return {};
    return catalog;
}
std::string Pseudolocalize(std::string_view text)
{
    std::string result = "[!! ";
    size_t letters = 0;
    for (const unsigned char character : text)
    {
        switch (character)
        {
        case 'a': result += "á"; break; case 'e': result += "ë"; break;
        case 'i': result += "ï"; break; case 'o': result += "ö"; break;
        case 'u': result += "ü"; break; case 'A': result += "Á"; break;
        case 'E': result += "Ë"; break; case 'I': result += "Ï"; break;
        case 'O': result += "Ö"; break; case 'U': result += "Ü"; break;
        default: result += static_cast<char>(character); break;
        }
        if ((character & 0xc0) != 0x80) ++letters;
    }
    result += std::string((letters + 2) / 3, '~');
    return result + " !!]";
}
}
