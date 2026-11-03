#include "Localization/MessageCheck.h"
#include <unicode/locid.h>
#include <unicode/messagepattern.h>
#include <unicode/msgfmt.h>
#include <unicode/plurrule.h>
#include <unicode/strenum.h>
#include <unicode/unistr.h>
#include <algorithm>
#include <memory>
#include <limits>

namespace GameEngine::Localization
{
namespace
{
constexpr const char* kPluralExample = "{count, plural, one {# coin} other {# coins}}";

icu::UnicodeString Utf16(std::string_view text)
{
    return icu::UnicodeString::fromUTF8(icu::StringPiece(text.data(), static_cast<int32_t>(text.size())));
}

bool IsContinuationByte(char byte)
{
    return (static_cast<unsigned char>(byte) & 0xC0u) == 0x80u;
}

// A brace hint for patterns ICU has already rejected. This scan handles common
// apostrophe escapes; ICU remains authoritative for full quoting semantics.
// `unclosed` counts remaining opening braces for the suggested repair.
std::optional<MessageProblem> FindUnbalancedBrace(std::string_view pattern, size_t& unclosed)
{
    std::vector<int32_t> open;
    bool quoted = false;
    int32_t character = 0;
    for (size_t index = 0; index < pattern.size(); ++index)
    {
        const char byte = pattern[index];
        if (IsContinuationByte(byte)) continue;
        ++character;
        if (byte == '\'')
        {
            const char next = index + 1 < pattern.size() ? pattern[index + 1] : '\0';
            if (next == '\'') { ++index; ++character; continue; }
            if (quoted || next == '{' || next == '}') quoted = !quoted;
            continue;
        }
        if (quoted) continue;
        if (byte == '{') open.push_back(character);
        else if (byte == '}')
        {
            if (open.empty())
                return MessageProblem{character, "a } has no opening {", "Remove it, or quote it as '}' to show it as text."};
            open.pop_back();
        }
    }
    if (open.empty()) return std::nullopt;
    unclosed = open.size();
    return MessageProblem{open.front(), "a { is never closed", {}};
}

// The example is the author's own text with every open brace closed at the end, shown only when
// that text is valid: closing the braces does not add a missing case.
std::string UnclosedBraceFix(std::string_view pattern, size_t unclosed, std::string_view locale)
{
    const std::string closed = std::string(pattern) + std::string(unclosed, '}');
    UErrorCode status = U_ZERO_ERROR;
    const auto language = icu::Locale::forLanguageTag(
        icu::StringPiece(locale.data(), static_cast<int32_t>(locale.size())), status);
    if (U_FAILURE(status)) return "Close it with } where the argument ends.";
    UParseError parseError{};
    const icu::MessageFormat format(Utf16(closed), language, parseError, status);
    if (U_FAILURE(status)) return "Close it with } where the argument ends.";
    return "Close it with }, e.g. " + closed;
}

MessageProblem DescribeIcuError(UErrorCode status, int32_t character)
{
    switch (status)
    {
    case U_DEFAULT_KEYWORD_MISSING:
        return {character, "the plural or select has no other {...} case",
                std::string("Add one, e.g. ") + kPluralExample + "."};
    case U_DUPLICATE_KEYWORD:
        return {character, "a plural or select case appears twice", "Keep one of the two cases."};
    case U_UNDEFINED_KEYWORD:
        return {character, "a plural case is not one of zero, one, two, few, many, other or =N",
                "Use those case names only."};
    case U_ARGUMENT_TYPE_MISMATCH:
    case U_ILLEGAL_ARGUMENT_ERROR:
        return {character, "an argument is not written as {name} or {name, type, ...}",
                std::string("Write it like {name} or ") + kPluralExample + "."};
    default:
        return {character, "the text is not valid ICU MessageFormat",
                std::string("Write arguments as {name} and plurals as ") + kPluralExample + "."};
    }
}
}

std::optional<MessageProblem> CheckMessage(std::string_view pattern, std::string_view locale)
{
    if (pattern.size() >= static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
        locale.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return MessageProblem{0, "the message or language tag is too long", "Split the message into smaller entries."};
    const auto text = Utf16(pattern);
    UErrorCode status = U_ZERO_ERROR;
    const auto language = icu::Locale::forLanguageTag(icu::StringPiece(locale.data(), static_cast<int32_t>(locale.size())), status);
    if (U_FAILURE(status)) return MessageProblem{0, "the language tag is not valid", "Use a language tag such as fr, pt-BR or zh-Hant."};
    UParseError parseError{};
    icu::MessageFormat format(text, language, parseError, status);
    if (U_SUCCESS(status)) return std::nullopt;
    // ICU owns quoting semantics, including '#' inside plural branches. Only
    // refine a real parse error; the brace hint must never reject valid text.
    size_t unclosed = 0;
    if (auto brace = FindUnbalancedBrace(pattern, unclosed))
    {
        if (unclosed > 0) brace->Fix = UnclosedBraceFix(pattern, unclosed, locale);
        return brace;
    }
    const int32_t offset = std::clamp(parseError.offset, 0, text.length());
    // ICU reports a UTF-16 offset; authors count characters from 1.
    return DescribeIcuError(status, offset > 0 ? text.countChar32(0, offset) + 1 : 0);
}

std::vector<std::string> MessageArgumentNames(std::string_view pattern)
{
    if (pattern.size() >= static_cast<size_t>(std::numeric_limits<int32_t>::max())) return {};
    UErrorCode status = U_ZERO_ERROR;
    UParseError parseError{};
    icu::MessagePattern parsed(Utf16(pattern), &parseError, status);
    if (U_FAILURE(status)) return {};
    std::vector<std::string> names;
    for (int32_t index = 0; index < parsed.countParts(); ++index)
    {
        const auto& part = parsed.getPart(index);
        if (part.getType() != UMSGPAT_PART_TYPE_ARG_NAME && part.getType() != UMSGPAT_PART_TYPE_ARG_NUMBER) continue;
        std::string name;
        parsed.getSubstring(part).toUTF8String(name);
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

namespace
{
// The locale's plural category names of `type`, or empty when ICU has no rules for it.
std::vector<std::string> PluralKeywords(const icu::Locale& locale, UPluralType type)
{
    UErrorCode status = U_ZERO_ERROR;
    const std::unique_ptr<icu::PluralRules> rules(icu::PluralRules::forLocale(locale, type, status));
    if (U_FAILURE(status) || !rules) return {};
    const std::unique_ptr<icu::StringEnumeration> keywords(rules->getKeywords(status));
    if (U_FAILURE(status) || !keywords) return {};
    std::vector<std::string> names;
    while (const icu::UnicodeString* keyword = keywords->snext(status))
    {
        if (U_FAILURE(status)) return {};
        std::string name;
        keyword->toUTF8String(name);
        names.push_back(std::move(name));
    }
    return names;
}
}

std::vector<std::string> MissingPluralCategories(std::string_view pattern, std::string_view locale)
{
    if (pattern.size() >= static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
        locale.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) return {};
    if (pattern.find("plural") == std::string_view::npos && pattern.find("selectordinal") == std::string_view::npos)
        return {};
    UErrorCode status = U_ZERO_ERROR;
    UParseError parseError{};
    icu::MessagePattern parsed(Utf16(pattern), &parseError, status);
    if (U_FAILURE(status)) return {};
    const auto language = icu::Locale::forLanguageTag(icu::StringPiece(locale.data(), static_cast<int32_t>(locale.size())), status);
    if (U_FAILURE(status)) return {};
    std::vector<std::string> cardinal;
    std::vector<std::string> ordinal;
    std::vector<std::string> missing;
    for (int32_t index = 0; index < parsed.countParts(); ++index)
    {
        const auto& start = parsed.getPart(index);
        if (start.getType() != UMSGPAT_PART_TYPE_ARG_START) continue;
        const bool isOrdinal = start.getArgType() == UMSGPAT_ARG_TYPE_SELECTORDINAL;
        if (start.getArgType() != UMSGPAT_ARG_TYPE_PLURAL && !isOrdinal) continue;
        auto& keywords = isOrdinal ? ordinal : cardinal;
        if (keywords.empty()) keywords = PluralKeywords(language, isOrdinal ? UPLURAL_TYPE_ORDINAL : UPLURAL_TYPE_CARDINAL);
        // This argument's own cases: nested messages are skipped here and visited on their own.
        std::vector<std::string> cases;
        const int32_t limit = parsed.getLimitPartIndex(index);
        for (int32_t part = index + 1; part < limit; ++part)
        {
            const auto& current = parsed.getPart(part);
            if (current.getType() == UMSGPAT_PART_TYPE_MSG_START) part = parsed.getLimitPartIndex(part);
            else if (current.getType() == UMSGPAT_PART_TYPE_ARG_SELECTOR)
            {
                std::string name;
                parsed.getSubstring(current).toUTF8String(name);
                cases.push_back(std::move(name));
            }
        }
        for (const auto& keyword : keywords)
            if (std::find(cases.begin(), cases.end(), keyword) == cases.end()) missing.push_back(keyword);
    }
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
    return missing;
}
}
