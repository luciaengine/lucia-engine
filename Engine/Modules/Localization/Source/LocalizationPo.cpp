#include "Localization/LocalizationDocument.h"
#include <sstream>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace GameEngine::Localization
{
namespace
{
std::string Quote(const std::string& value)
{
    std::string result = "\"";
    for (unsigned char character : value)
    {
        switch (character)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 32 || character == 127)
            {
                result += '\\';
                result += static_cast<char>('0' + ((character >> 6) & 7));
                result += static_cast<char>('0' + ((character >> 3) & 7));
                result += static_cast<char>('0' + (character & 7));
            }
            else result += static_cast<char>(character);
        }
    }
    return result + '"';
}
void Comment(std::ostream& output, const char* prefix, const std::string& value)
{
    std::istringstream lines(value);
    std::string line;
    while (std::getline(lines, line)) output << prefix << line << '\n';
}
std::string Unquote(std::string_view value)
{
    const auto start = value.find('"');
    const auto end = value.find_last_not_of(" \t\r");
    if (start == std::string_view::npos || end <= start || value[end] != '"')
        throw std::runtime_error("Expected a quoted PO string.");
    if (value.substr(0, start).find_first_not_of(" \t") != std::string_view::npos)
        throw std::runtime_error("Unexpected text before PO string.");
    std::string result;
    for (size_t i = start + 1; i < end; ++i)
    {
        char c = value[i];
        if (c == '"') throw std::runtime_error("Unescaped quote in PO string.");
        if (c == '\\')
        {
            if (++i >= end) throw std::runtime_error("Incomplete PO escape.");
            switch (value[i])
            {
            case 'a': c = '\a'; break; case 'b': c = '\b'; break;
            case 'f': c = '\f'; break; case 'v': c = '\v'; break;
            case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
            case '\\': c = '\\'; break; case '"': c = '"'; break;
            default:
                if (value[i] < '0' || value[i] > '7')
                    throw std::runtime_error("Unsupported PO escape; use UTF-8 text or C escapes.");
                unsigned int byte = value[i] - '0';
                for (int digit = 1; digit < 3 && i + 1 < end && value[i + 1] >= '0' && value[i + 1] <= '7'; ++digit)
                    byte = byte * 8 + (value[++i] - '0');
                if (byte > 255) throw std::runtime_error("PO octal escape exceeds one byte.");
                c = static_cast<char>(byte);
                break;
            }
        }
        result += c;
    }
    return result;
}
struct PoEntry
{
    std::string Key, Source, Translation;
    bool HasKey = false, HasSource = false, HasTranslation = false, Fuzzy = false;
    // 1-based line of the entry's first comment or directive, for errors found after parsing.
    size_t Line = 0;
};
}
bool LocalizationDocument::ExportPo(std::ostream& output, const std::string& locale, std::string& error) const
{
    if (std::find(m_Locales.begin(), m_Locales.end(), locale) == m_Locales.end())
    { error = "Add the target locale before exporting."; return false; }
    output << "msgid \"\"\nmsgstr " << Quote("Language: " + locale + "\nContent-Type: text/plain; charset=UTF-8\nX-Generator: GameEngine\n") << "\n\n";
    for (const auto& [id, entry] : m_Entries)
    {
        const auto source = entry.Translations.find("en");
        if (source == entry.Translations.end()) continue;
        Comment(output, "#. Context: ", entry.Context);
        Comment(output, "#. ", entry.Notes);
        const auto translation = entry.Translations.find(locale);
        output << "msgctxt " << Quote(entry.Key) << "\nmsgid " << Quote(source->second)
               << "\nmsgstr " << Quote(translation == entry.Translations.end() ? "" : translation->second) << "\n\n";
    }
    if (!output) { error = "Could not write PO file."; return false; }
    error.clear(); return true;
}
bool LocalizationDocument::ImportPo(std::istream& input, const std::string& locale, std::string& error)
{
    auto candidate = *this;
    if (!candidate.AddLocale(locale, error)) return false;
    LocalizationCatalog normalizer;
    if (!normalizer.SetLocale(locale, error)) return false;
    const auto target = normalizer.GetLocale();
    if (target == "en") { error = "Import into a translation locale, not the English source."; return false; }
    std::vector<PoEntry> entries;
    PoEntry current;
    std::string* continuation = nullptr;
    size_t lineNumber = 0;
    try
    {
        std::string line;
        while (std::getline(input, line))
        {
            ++lineNumber;
            const auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos)
            {
                if (current.HasSource) entries.push_back(std::move(current));
                else if (current.HasKey || current.HasTranslation) throw std::runtime_error("Incomplete PO entry.");
                current = {}; continuation = nullptr; continue;
            }
            line.erase(0, first);
            if (current.HasTranslation && (line.starts_with("#") || line.starts_with("msgctxt ") || line.starts_with("msgid ")))
            { entries.push_back(std::move(current)); current = {}; continuation = nullptr; }
            if (current.Line == 0) current.Line = lineNumber;
            if (line.starts_with("#~")) continue;
            if (line.starts_with("#"))
            {
                if (line.starts_with("#,") && line.find("fuzzy") != std::string::npos) current.Fuzzy = true;
                continue;
            }
            if (line.starts_with("msgid_plural") || line.starts_with("msgstr["))
                throw std::runtime_error("Use one ICU MessageFormat pattern per entry; gettext plural arrays are not supported.");
            if (line.starts_with("msgctxt "))
            {
                if (current.HasKey || current.HasSource) throw std::runtime_error("Duplicate context or missing entry separator.");
                current.HasKey = true; continuation = &current.Key;
                *continuation = Unquote(std::string_view(line).substr(8));
            }
            else if (line.starts_with("msgid "))
            {
                if (current.HasSource) throw std::runtime_error("Duplicate msgid or missing entry separator.");
                current.HasSource = true; continuation = &current.Source;
                *continuation = Unquote(std::string_view(line).substr(6));
            }
            else if (line.starts_with("msgstr "))
            {
                if (!current.HasSource || current.HasTranslation) throw std::runtime_error("Unexpected msgstr.");
                current.HasTranslation = true; continuation = &current.Translation;
                *continuation = Unquote(std::string_view(line).substr(7));
            }
            else if (line.starts_with('"') && continuation) *continuation += Unquote(line);
            else throw std::runtime_error("Unrecognized PO directive.");
        }
        if (input.bad()) throw std::runtime_error("Could not read PO file.");
        if (current.HasSource) entries.push_back(std::move(current));
        else if (current.HasKey || current.HasTranslation) throw std::runtime_error("Incomplete PO entry.");
        std::set<StringId> seen;
        bool headerSeen = false;
        for (const auto& entry : entries)
        {
            // Errors from here on name the entry's own line, not the file's last.
            lineNumber = entry.Line;
            if (!entry.HasTranslation) throw std::runtime_error("Entry has no msgstr.");
            if (!entry.HasKey && entry.Source.empty())
            {
                if (headerSeen) throw std::runtime_error("Duplicate PO header.");
                headerSeen = true;
                std::istringstream header(entry.Translation);
                std::string field;
                while (std::getline(header, field))
                {
                    if (field.starts_with("Language:"))
                    {
                        auto language = field.substr(9);
                        language.erase(0, language.find_first_not_of(" \t"));
                        if (!normalizer.SetLocale(language, error) || normalizer.GetLocale() != target)
                            throw std::runtime_error("PO Language header differs from selected locale.");
                    }
                    if (field.starts_with("Content-Type:") && field.find("charset=UTF-8") == std::string::npos)
                        throw std::runtime_error("PO files must use UTF-8.");
                }
                continue;
            }
            const auto id = HashStringId(entry.Key);
            const auto found = candidate.m_Entries.find(id);
            if (!entry.HasKey || found == candidate.m_Entries.end() || found->second.Key != entry.Key)
                throw std::runtime_error("Unknown translation key in msgctxt: " + entry.Key);
            if (!seen.insert(id).second) throw std::runtime_error("Duplicate PO key: " + entry.Key);
            const auto source = found->second.Translations.find("en");
            if (source == found->second.Translations.end() || source->second != entry.Source)
                throw std::runtime_error("English source changed; re-export before importing key: " + entry.Key);
            // Unreviewed and untranslated entries never erase a valid translation.
            if (!entry.Fuzzy && !entry.Translation.empty()) found->second.Translations[target] = entry.Translation;
        }
        lineNumber = 0;
        if (entries.empty()) throw std::runtime_error("PO file contains no entries.");
        if (!candidate.Validate(error)) return false;
        *this = std::move(candidate); error.clear(); return true;
    }
    catch (const std::exception& exception)
    {
        error = (lineNumber > 0 ? "PO near line " + std::to_string(lineNumber) + ": " : std::string("PO file: ")) + exception.what();
        return false;
    }
}
}
