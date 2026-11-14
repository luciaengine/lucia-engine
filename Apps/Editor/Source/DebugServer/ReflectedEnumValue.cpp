#include "DebugServer/ReflectedEnumValue.h"

#include "ECS/Reflection.h"
#include "Types/StringUtils.h"

#include <charconv>
#include <limits>
#include <string>

namespace GameEngine::Editor
{

std::optional<std::int64_t> ResolveEnumFieldValue(const ECS::FieldInfo& field, const nlohmann::json& value)
{
    std::int64_t number = 0;
    if (value.is_number_unsigned())
    {
        const auto unsignedValue = value.get<std::uint64_t>();
        if (unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return std::nullopt;
        number = static_cast<std::int64_t>(unsignedValue);
    }
    else if (value.is_number_integer())
    {
        number = value.get<std::int64_t>();
    }
    else if (value.is_string())
    {
        const std::string& name = value.get_ref<const std::string&>();
        for (const ECS::EnumNameValue& e : field.EnumNames)
        {
            if (EqualsIgnoreCase(e.Name, name))
                return e.Value;
        }
        const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), number);
        if (ec != std::errc{} || ptr != name.data() + name.size())
            return std::nullopt;
    }
    else
    {
        return std::nullopt;
    }

    for (const ECS::EnumNameValue& e : field.EnumNames)
    {
        if (e.Value == number)
            return number;
    }
    return std::nullopt;
}

std::string DescribeEnumFieldValues(const ECS::FieldInfo& field)
{
    std::string text;
    for (const ECS::EnumNameValue& e : field.EnumNames)
    {
        if (!text.empty())
            text += ", ";
        text.append(e.Name);
        text += '=';
        text += std::to_string(e.Value);
    }
    return text;
}

} // namespace GameEngine::Editor
