#include <gtest/gtest.h>

#include "DebugServer/ReflectedEnumValue.h"

#include "ECS/Reflection.h"

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <limits>

namespace
{
using GameEngine::ECS::EnumNameValue;
using GameEngine::ECS::FieldInfo;
using GameEngine::ECS::FieldTypeId;
using GameEngine::Editor::DescribeEnumFieldValues;
using GameEngine::Editor::ResolveEnumFieldValue;
using json = nlohmann::json;

// A uint8 enum with a gap, shaped like ValueCurve.Target: 0..2 and 8 are declared.
constexpr std::array<EnumNameValue, 4> kGappedEnumTargets{{
    {"None", 0},
    {"TransformPositionX", 1},
    {"TransformPositionY", 2},
    {"PostProcessWeight", 8},
}};

FieldInfo GappedEnumTargetField()
{
    FieldInfo field{"Target", 0, 1, FieldTypeId::UInt8};
    field.EnumNames = kGappedEnumTargets;
    return field;
}

// The defect this locks: set_component stored any integer that fit the field, so
// ValueCurve.Target = 9 or 200 answered `ok` and left a value the enum does not declare.
TEST(ReflectedEnumValueTests, RefusesAnIntegerTheEnumDoesNotDeclare)
{
    const FieldInfo field = GappedEnumTargetField();
    EXPECT_EQ(ResolveEnumFieldValue(field, json(9)), std::nullopt);
    EXPECT_EQ(ResolveEnumFieldValue(field, json(200)), std::nullopt);
    EXPECT_EQ(ResolveEnumFieldValue(field, json(-1)), std::nullopt);
    EXPECT_EQ(ResolveEnumFieldValue(field, json("9")), std::nullopt);
    EXPECT_EQ(ResolveEnumFieldValue(field, json(1.5)), std::nullopt);
}

TEST(ReflectedEnumValueTests, AcceptsADeclaredIntegerAsNumberOrString)
{
    const FieldInfo field = GappedEnumTargetField();
    EXPECT_EQ(ResolveEnumFieldValue(field, json(8)), 8);
    EXPECT_EQ(ResolveEnumFieldValue(field, json("2")), 2);
}

TEST(ReflectedEnumValueTests, ResolvesNamesCaseInsensitively)
{
    const FieldInfo field = GappedEnumTargetField();
    EXPECT_EQ(ResolveEnumFieldValue(field, json("postprocessweight")), 8);
    EXPECT_EQ(ResolveEnumFieldValue(field, json("NoSuchTarget")), std::nullopt);
}

TEST(ReflectedEnumValueTests, DescribesEveryEnumeratorWithItsValue)
{
    EXPECT_EQ(DescribeEnumFieldValues(GappedEnumTargetField()),
              "None=0, TransformPositionX=1, TransformPositionY=2, PostProcessWeight=8");
}

TEST(ReflectedEnumValueTests, RefusesAnUnsignedValueThatWouldWrapToAnEnumerator)
{
    constexpr std::array<EnumNameValue, 1> entries{{{"Negative", -1}}};
    FieldInfo field{"Value", 0, sizeof(std::int64_t), FieldTypeId::Int64};
    field.EnumNames = entries;
    EXPECT_EQ(ResolveEnumFieldValue(field, json(std::numeric_limits<std::uint64_t>::max())), std::nullopt);
    EXPECT_EQ(ResolveEnumFieldValue(field, json(-1)), -1);
}

} // namespace
