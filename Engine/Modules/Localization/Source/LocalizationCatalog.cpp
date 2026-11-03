#include "Localization/LocalizationCatalog.h"
#include <nlohmann/json.hpp>
#include <unicode/locid.h>
#include <unicode/msgfmt.h>
#include <unicode/messagepattern.h>
#include <unicode/uloc.h>
#include <unicode/ustring.h>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace GameEngine::Localization
{
namespace
{
std::string NormalizeLocale(std::string locale)
{
    if (locale.empty()) throw std::runtime_error("Locale must not be empty.");
    std::replace(locale.begin(), locale.end(), '_', '-');
    UErrorCode status = U_ZERO_ERROR;
    char name[ULOC_FULLNAME_CAPACITY];
    int32_t parsed = 0;
    uloc_forLanguageTag(locale.c_str(), name, sizeof(name), &parsed, &status);
    if (U_FAILURE(status) || parsed != static_cast<int32_t>(locale.size()))
        throw std::runtime_error("Invalid language tag: " + locale);
    auto result = icu::Locale::createCanonical(name).toLanguageTag<std::string>(status);
    if (U_FAILURE(status)) throw std::runtime_error(u_errorName(status));
    return result;
}
icu::UnicodeString Utf8(std::string_view text)
{
    if (text.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        throw std::runtime_error("Message exceeds ICU size limit.");
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = 0;
    u_strFromUTF8(nullptr, 0, &length, text.data(), static_cast<int32_t>(text.size()), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status))
        throw std::runtime_error("Message contains invalid UTF-8.");
    return icu::UnicodeString::fromUTF8(icu::StringPiece(text.data(), static_cast<int32_t>(text.size())));
}
std::vector<std::string> LocaleChain(const std::string& tag)
{
    UErrorCode status = U_ZERO_ERROR;
    auto locale = icu::Locale::forLanguageTag(tag, status);
    if (U_FAILURE(status)) throw std::runtime_error(u_errorName(status));
    std::vector<std::string> chain;
    auto append = [&](const icu::Locale& candidate)
    {
        const auto value = candidate.toLanguageTag<std::string>(status);
        if (U_FAILURE(status)) throw std::runtime_error(u_errorName(status));
        if (std::find(chain.begin(), chain.end(), value) == chain.end()) chain.push_back(value);
    };
    auto inferred = locale;
    inferred.addLikelySubtags(status);
    if (U_FAILURE(status)) throw std::runtime_error(u_errorName(status));
    const std::string languageScript = std::string(inferred.getLanguage()) + "_" + inferred.getScript();
    std::string name = locale.getBaseName();
    while (!name.empty())
    {
        const icu::Locale candidate(name.c_str());
        if (name == locale.getBaseName()) append(candidate);
        // Prefer a script-specific translation over a generic language table.
        // Keep exact region/variant matches first; never invent a default region.
        if (name == locale.getLanguage() && inferred.getScript()[0] != '\0')
        {
            if (locale.getCountry()[0] != '\0') append(inferred);
            append(icu::Locale(languageScript.c_str()));
        }
        append(candidate);
        char parent[ULOC_FULLNAME_CAPACITY];
        uloc_getParent(name.c_str(), parent, sizeof(parent), &status);
        if (U_FAILURE(status)) throw std::runtime_error(u_errorName(status));
        name = parent;
    }
    if (std::find(chain.begin(), chain.end(), "en") == chain.end()) chain.push_back("en");
    return chain;
}
}
struct LocalizationCatalog::State
{
    struct Message
    {
        std::unique_ptr<icu::MessageFormat> Format;
        std::set<std::string> Arguments;
    };
    using Table = std::map<StringId, Message>;
    std::map<StringId, Table> Tables;
    std::string Locale = "en";
    std::vector<std::string> Chain = {"en"};
};
LocalizationCatalog::LocalizationCatalog() : m_State(std::make_unique<State>()) {}
LocalizationCatalog::~LocalizationCatalog() = default;

bool LocalizationCatalog::Load(std::istream& input, std::string& error)
{
    decltype(m_State->Tables) tables;
    std::map<StringId, std::string> keyNames;
    try
    {
        // nlohmann's default DOM silently keeps the last duplicate member.
        // Reject duplicates before building it so translator mistakes stay visible.
        std::vector<std::set<std::string>> members;
        auto callback = [&members](int, nlohmann::json::parse_event_t event, nlohmann::json& value)
        {
            if (event == nlohmann::json::parse_event_t::object_start) members.emplace_back();
            else if (event == nlohmann::json::parse_event_t::object_end) members.pop_back();
            else if (event == nlohmann::json::parse_event_t::key &&
                     !members.back().insert(value.get<std::string>()).second)
                throw std::runtime_error("Duplicate translation member: " + value.get<std::string>());
            return true;
        };
        const auto json = nlohmann::json::parse(input, callback);
        if (!json.is_object()) throw std::runtime_error("Catalog must be an object of locale tables.");
        for (const auto& [locale, entries] : json.items())
        {
            if (locale == "$metadata")
            {
                if (!entries.is_object()) throw std::runtime_error("Metadata must be an object.");
                for (const auto& [key, metadata] : entries.items())
                {
                    if (key.empty() || !metadata.is_object()) throw std::runtime_error("Invalid translation metadata.");
                    for (const auto& [field, value] : metadata.items())
                        if ((field != "context" && field != "notes") || !value.is_string())
                            throw std::runtime_error("Metadata accepts only string context and notes.");
                }
                continue;
            }
            const auto normalized = NormalizeLocale(locale);
            if (!entries.is_object() || tables.contains(HashStringId(normalized)))
                throw std::runtime_error("Invalid or duplicate locale table: " + locale);
            auto& table = tables[HashStringId(normalized)];
            UErrorCode status = U_ZERO_ERROR;
            const auto language = icu::Locale::forLanguageTag(normalized, status);
            for (const auto& [key, value] : entries.items())
            {
                if (key.empty() || !value.is_string())
                    throw std::runtime_error("Translation requires a nonempty key and string value: " + key);
                const auto id = HashStringId(key);
                const auto [name, inserted] = keyNames.emplace(id, key);
                if (!inserted && name->second != key)
                    throw std::runtime_error("Translation key hash collision: " + key);
                UParseError parseError{};
                auto format = std::make_unique<icu::MessageFormat>(Utf8(value.get<std::string>()), language, parseError, status);
                if (U_FAILURE(status))
                    throw std::runtime_error(locale + ":" + key + " at " + std::to_string(parseError.offset) + ": " + u_errorName(status));
                icu::MessagePattern pattern(Utf8(value.get<std::string>()), &parseError, status);
                if (U_FAILURE(status)) throw std::runtime_error(u_errorName(status));
                std::set<std::string> arguments;
                for (int32_t index = 0; index < pattern.countParts(); ++index)
                {
                    const auto& part = pattern.getPart(index);
                    if (part.getType() == UMSGPAT_PART_TYPE_ARG_NAME || part.getType() == UMSGPAT_PART_TYPE_ARG_NUMBER)
                    {
                        std::string argumentName;
                        pattern.getSubstring(part).toUTF8String(argumentName);
                        arguments.insert(std::move(argumentName));
                    }
                }
                table.emplace(id, State::Message{std::move(format), std::move(arguments)});
            }
        }
        if (!tables.contains(HashStringId("en"))) throw std::runtime_error("Catalog requires an English (en) fallback table.");
        const auto& source = tables.at(HashStringId("en"));
        for (const auto& [locale, table] : tables)
            for (const auto& [key, message] : table)
                if (const auto english = source.find(key); english != source.end() && english->second.Arguments != message.Arguments)
                    throw std::runtime_error("Translation arguments differ from English for key: " + keyNames.at(key));
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
    m_State->Tables = std::move(tables);
    error.clear();
    Changed.Invoke();
    return true;
}

bool LocalizationCatalog::SetLocale(std::string locale, std::string& error)
{
    try
    {
        locale = NormalizeLocale(std::move(locale));
        auto chain = LocaleChain(locale);
        error.clear();
        if (locale == m_State->Locale) return true;
        m_State->Locale = std::move(locale);
        m_State->Chain = std::move(chain);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
    Changed.Invoke();
    return true;
}
bool LocalizationCatalog::SetSystemLocale(std::string& error)
{
    UErrorCode status = U_ZERO_ERROR;
    const auto language = icu::Locale::getDefault().toLanguageTag<std::string>(status);
    if (U_FAILURE(status))
    {
        error = u_errorName(status);
        return false;
    }
    return SetLocale(language, error);
}

bool LocalizationCatalog::RestoreLocale(std::istream& input, std::string& error)
{
    std::string language;
    // BCP-47 preferences are bounded, single-line values rather than arbitrary documents.
    char character;
    while (input.get(character))
    {
        if (character == '\n') break;
        if (language.size() == 255)
        {
            error = "Saved language tag exceeds 255 bytes.";
            return false;
        }
        language.push_back(character);
    }
    if (input.bad())
    {
        error = "Could not read saved language.";
        return false;
    }
    if (!language.empty() && language.back() == '\r') language.pop_back();
    return SetLocale(std::move(language), error);
}

bool LocalizationCatalog::WriteLocale(std::ostream& output, std::string& error) const
{
    output << GetLocale() << '\n';
    output.flush();
    if (!output)
    {
        error = "Could not save selected language.";
        return false;
    }
    error.clear();
    return true;
}

const std::string& LocalizationCatalog::GetLocale() const { return m_State->Locale; }

std::string LocalizationCatalog::Resolve(StringId key, std::string_view fallback,
                                         const MessageArguments& arguments, std::string* error) const
{
    if (error) error->clear();
    try
    {
        for (const auto& locale : m_State->Chain)
        {
            const auto table = m_State->Tables.find(HashStringId(locale));
            if (table == m_State->Tables.end()) continue;
            const auto entry = table->second.find(key);
            if (entry == table->second.end()) continue;
            std::vector<icu::UnicodeString> names;
            std::vector<icu::Formattable> values;
            for (const auto& [name, value] : arguments)
            {
                names.push_back(Utf8(name));
                std::visit([&values](const auto& v)
                {
                    if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) values.emplace_back(Utf8(v));
                    else values.emplace_back(v);
                }, value);
            }
            UErrorCode status = U_ZERO_ERROR;
            // ICU intentionally leaves missing arguments as placeholders. UI should
            // show the authored fallback instead of exposing message syntax.
            for (const auto& name : entry->second.Arguments)
                if (!arguments.contains(name)) throw std::runtime_error("Missing message argument: " + name);
            icu::UnicodeString result;
            entry->second.Format->format(names.data(), values.data(), static_cast<int32_t>(values.size()), result, status);
            if (U_FAILURE(status)) throw std::runtime_error(u_errorName(status));
            std::string utf8;
            result.toUTF8String(utf8);
            return utf8;
        }
        if (error) *error = "Translation key was not found.";
    }
    catch (const std::exception& exception)
    {
        if (error) *error = exception.what();
    }
    return std::string(fallback);
}

std::string LanguageDisplayName(std::string_view locale)
{
    if (locale.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) return std::string(locale);
    UErrorCode status = U_ZERO_ERROR;
    const auto language = icu::Locale::forLanguageTag(
        icu::StringPiece(locale.data(), static_cast<int32_t>(locale.size())), status);
    if (U_FAILURE(status) || language.isBogus()) return std::string(locale);
    icu::UnicodeString name;
    language.getDisplayName(icu::Locale::getEnglish(), name);
    std::string text;
    name.toUTF8String(text);
    return text.empty() ? std::string(locale) : text;
}
}
