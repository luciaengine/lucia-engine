#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace GameEngine::ECS
{
struct FieldInfo;
}

namespace GameEngine::Editor
{

// The integer a `set_component` value names for a reflected enum field (a field whose
// FieldInfo::EnumNames table is not empty): an enumerator NAME, the form the scene format and
// the inspector use (e.g. RenderMode="Fullscreen"), or an integer given as a JSON integer or as
// a numeric string (callers and scripts sometimes quote enum values). An integer must be the
// value of an enumerator in the table, so a value the enum does not declare (ValueCurve.Target =
// 9) is refused instead of stored.
//
// Names match case-INSENSITIVELY, like the field name the value arrived under and like the
// scene loader, which lowercases before comparing: "blend" loading from a .scene and failing
// over IPC is the get/set disagreement this surface exists to remove. ReflectedEnumNameDrift
// pins the precondition: no reflected enum has two enumerators differing only by case.
//
// Returns nullopt for a value that names no enumerator, so the caller reports the field as not
// applied instead of answering `ok` for a write that changed nothing.
std::optional<std::int64_t> ResolveEnumFieldValue(const ECS::FieldInfo& field, const nlohmann::json& value);

// The field's enumerators as "Name=Value" pairs in table order, joined by ", ", for an error
// that tells the caller which values the field accepts.
std::string DescribeEnumFieldValues(const ECS::FieldInfo& field);

} // namespace GameEngine::Editor
