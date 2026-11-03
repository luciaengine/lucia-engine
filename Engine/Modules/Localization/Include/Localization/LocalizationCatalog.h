#pragma once
#include "Events/Event.h"
#include "Types/StringId.h"
#include <cstdint>
#include <istream>
#include <ostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace GameEngine::Localization
{
// Names are ICU message placeholders, not engine object identities.
using MessageArguments = std::map<std::string, std::variant<std::string, int64_t, double>>;

// One shared catalog per UI context. Load, locale changes and notifications must
// run on the owning UI thread. Unicode shaping remains the Text module's job.
class LocalizationCatalog final
{
public:
    LocalizationCatalog();
    ~LocalizationCatalog();
    LocalizationCatalog(const LocalizationCatalog&) = delete;
    LocalizationCatalog& operator=(const LocalizationCatalog&) = delete;

    // UTF-8 JSON locale tables. Failed loads preserve the previous catalog.
    bool Load(std::istream& input, std::string& error);
    // Accepts BCP-47 tags and legacy underscore separators. Invalid tags fail
    // without changing the locale or notifying subscribers.
    bool SetLocale(std::string locale, std::string& error);
    const std::string& GetLocale() const;
    bool SetSystemLocale(std::string& error);
    // The host chooses its writable preference store; no runtime repository paths.
    bool RestoreLocale(std::istream& input, std::string& error);
    bool WriteLocale(std::ostream& output, std::string& error) const;
    // ICU evaluates messages using the locale of the resolved translation.
    // Missing keys or format errors return fallback; errors are optional diagnostics.
    std::string Resolve(StringId key, std::string_view fallback,
                        const MessageArguments& arguments = {}, std::string* error = nullptr) const;
    Event<> Changed;
private:
    struct State;
    std::unique_ptr<State> m_State;
};

// The English name of a BCP-47 language tag for lists a developer reads, e.g. "Dutch" for nl
// and "Portuguese (Brazil)" for pt-BR; the tag itself when ICU has no name for it.
std::string LanguageDisplayName(std::string_view locale);
}
