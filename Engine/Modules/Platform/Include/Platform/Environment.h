#pragma once

#include "Types/StringUtils.h"

#include <cstdlib>
#include <initializer_list>
#include <string_view>

namespace GameEngine::Platform
{

/// Whether `value` is one of the off values every environment switch shares:
/// "0", "false", "off" or "no", in any case.
inline bool IsEnvironmentSwitchOffValue(std::string_view value)
{
    for (const std::string_view off : {"0", "false", "off", "no"})
    {
        if (EqualsIgnoreCase(value, off))
            return true;
    }
    return false;
}

/// Read a developer switch from the process environment.
///
/// Unset or empty returns `valueWhenUnset`. The off values
/// (IsEnvironmentSwitchOffValue) are off; any other value is on. The web build
/// reaches the same values through the query string
/// (Platform/WebEnvironment.h), so a switch works the same way on both.
inline bool EnvironmentSwitchEnabled(const char* name, bool valueWhenUnset)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
        return valueWhenUnset;
    return !IsEnvironmentSwitchOffValue(value);
}

} // namespace GameEngine::Platform
