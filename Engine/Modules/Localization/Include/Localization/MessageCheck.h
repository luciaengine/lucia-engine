#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Localization
{
// Why an ICU message pattern does not compile, in words an author can act on.
struct MessageProblem
{
    // 1-based character (code point) position in the pattern; 0 when the problem has none.
    int32_t Character = 0;
    // What is wrong, as a clause: "a { is never closed".
    std::string Problem;
    // How to fix it, as a sentence: "Close it with }, e.g. ...".
    std::string Fix;
};

// Compiles `pattern` as an ICU MessageFormat message for the BCP-47 `locale` and returns its
// first problem, or nothing when it compiles.
std::optional<MessageProblem> CheckMessage(std::string_view pattern, std::string_view locale);

// The argument names `pattern` uses, sorted and unique; empty when it does not compile.
std::vector<std::string> MessageArgumentNames(std::string_view pattern);

// The CLDR plural categories of the BCP-47 `locale` (one, few, many, ...) that a plural or
// selectordinal argument in `pattern` has no case for, sorted and unique. Explicit =N cases do
// not stand in for a category. Empty when every category is covered or the pattern does not
// compile.
std::vector<std::string> MissingPluralCategories(std::string_view pattern, std::string_view locale);
}
