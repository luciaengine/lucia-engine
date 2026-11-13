// The Terrain Grass Preset row (Terrain/TerrainGrass/TerrainGrassPresetRow): the list holds the four presets, each
// with a picture; the field reads Custom once a dial differs from the preset, without a
// Custom entry; a pick applies the preset as one undo step; and the pictures were taken with the
// presets' values.

#include "Terrain/TerrainGrass/TerrainGrassPresetRow.h"

#include "AssetCore/SharedFileRead.h"
#include "Components/Terrain/TerrainGrass.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "TerrainGrass/TerrainGrassPresets.h"
#include "Types/ParseNumber.h"
#include "Types/StringUtils.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>

using namespace GameEngine;

namespace
{

Dropdown* FindDropdown(UIElement& element)
{
    if (auto* dropdown = dynamic_cast<Dropdown*>(&element))
        return dropdown;
    for (const auto& child : element.GetChildren())
    {
        if (Dropdown* found = FindDropdown(*child))
            return found;
    }
    return nullptr;
}

UIElement* ChildWithClass(const UIElement& parent, const char* className)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

// One `key=value` of a picture's line in grass-pictures.txt, or empty when the line has no such key.
std::string_view RecordValue(std::string_view line, std::string_view key)
{
    for (std::string_view token : Split(line, ' '))
    {
        const size_t equals = token.find('=');
        if (equals != std::string_view::npos && token.substr(0, equals) == key)
            return token.substr(equals + 1);
    }
    return {};
}

void ExpectRecordedNumber(std::string_view line, std::string_view key, float expected)
{
    const std::optional<float> recorded = ParseFloat(RecordValue(line, key));
    ASSERT_TRUE(recorded.has_value()) << "no number for " << key << " in: " << line;
    EXPECT_NEAR(*recorded, expected, 1e-4f) << key << " in: " << line;
}

// A color as the record writes it: the stored sRGB word in hex, or "auto" for the automatic root color.
std::string RecordedColor(uint32 color)
{
    return color == Components::kTerrainGrassAutoRootColor ? std::string("auto") : std::format("{:08X}", color);
}

// The record's key for each look dial: the scene file's field name.
template <typename Field>
struct RecordedDial
{
    Field Components::TerrainGrass::*Dial;
    const char* Key;
};

constexpr RecordedDial<float32> kRecordedFloatDials[] = {
    {&Components::TerrainGrass::BladesPerSquareMeter, "bladesPerSquareMeter"},
    {&Components::TerrainGrass::ClumpSize, "clumpSize"},
    {&Components::TerrainGrass::ClumpHeightVariance, "clumpHeightVariance"},
    {&Components::TerrainGrass::ClumpAlignment, "clumpAlignment"},
    {&Components::TerrainGrass::ClumpTaper, "clumpTaper"},
    {&Components::TerrainGrass::BladeHeight, "bladeHeight"},
    {&Components::TerrainGrass::BladeWidth, "bladeWidth"},
    {&Components::TerrainGrass::BladeTilt, "bladeTilt"},
    {&Components::TerrainGrass::BladeBend, "bladeBend"},
    {&Components::TerrainGrass::BladeTwist, "bladeTwist"},
    {&Components::TerrainGrass::BladeTaperStart, "bladeTaperStart"},
    {&Components::TerrainGrass::TuftFan, "tuftFan"},
    {&Components::TerrainGrass::RandomScale, "randomScale"},
    {&Components::TerrainGrass::WindRestingLean, "windRestingLean"},
    {&Components::TerrainGrass::WindFlutterAmount, "windFlutterAmount"},
    {&Components::TerrainGrass::WindFlutterSpeed, "windFlutterSpeed"},
    {&Components::TerrainGrass::Brightness, "brightness"},
    {&Components::TerrainGrass::RandomBrightness, "randomBrightness"},
    {&Components::TerrainGrass::HueVariation, "hueVariation"},
    {&Components::TerrainGrass::RootFadeStart, "rootFadeStart"},
    {&Components::TerrainGrass::RootFadeEnd, "rootFadeEnd"},
    {&Components::TerrainGrass::GroundingStrength, "groundingStrength"},
    {&Components::TerrainGrass::GroundTintHeight, "groundTintHeight"},
    {&Components::TerrainGrass::CanopyShade, "canopyShade"},
    {&Components::TerrainGrass::Translucency, "translucency"},
};

constexpr RecordedDial<uint32> kRecordedColorDials[] = {
    {&Components::TerrainGrass::RootColor, "rootColor"},
    {&Components::TerrainGrass::TipColor, "tipColor"},
    {&Components::TerrainGrass::BacklightColor, "backlightColor"},
};

template <typename Field, size_t Count>
const char* RecordKey(const RecordedDial<Field> (&dials)[Count], Field Components::TerrainGrass::*dial)
{
    for (const RecordedDial<Field>& recorded : dials)
    {
        if (recorded.Dial == dial)
            return recorded.Key;
    }
    return nullptr;
}

std::string ShownText(const Dropdown& presets)
{
    return presets.GetHeaderLabel() ? presets.GetHeaderLabel()->GetText() : std::string();
}

// A terrain with default grass (Meadow) and its Preset row.
struct GrassRow
{
    ECS::World World;
    Editor::EditorChangeNotifications Notifications;
    Editor::UndoRedoService Undo;
    UIElement Parent;
    ECS::EntityHandle Terrain{};
    Dropdown* Presets = nullptr;
    int Refreshes = 0;

    GrassRow()
    {
        Terrain = World.CreateEntity();
        World.AddComponentImmediate(Terrain, Components::TerrainGrass{});
        AddTerrainGrassPresetRow(&Parent, &World, Terrain, &Notifications, &Undo, [this]() { ++Refreshes; });
        Presets = FindDropdown(Parent);
    }

    const Components::TerrainGrass& Grass() { return *World.GetComponent<Components::TerrainGrass>(Terrain); }

    // A dial edited the way the inspector's rows edit one: written, reconciled and committed.
    void SetBladeHeight(float height)
    {
        Components::TerrainGrass updated = Grass();
        updated.BladeHeight = height;
        TerrainGrass::ReconcileTerrainGrassPreset(updated);
        World.AddComponentImmediate(Terrain, updated);
        Notifications.NotifyComponentCommit<Components::TerrainGrass>(&World, Terrain);
    }
};

} // namespace

// Meadow, Lawn, Steppe and Stylized in that order, each with its picture; Custom is not an entry.
TEST(TerrainGrassPresetRow, TheListIsTheFourPresetsEachWithAPicture)
{
    GrassRow row;
    ASSERT_NE(row.Presets, nullptr);
    EXPECT_EQ(row.Presets->GetSelectedIndex(), 0) << "new grass is Meadow";
    EXPECT_EQ(ShownText(*row.Presets), "Meadow");
    const char* names[] = {"Meadow", "Lawn", "Steppe", "Stylized"};
    const char* pictures[] = {"option-picture--grass-meadow", "option-picture--grass-lawn",
                              "option-picture--grass-steppe", "option-picture--grass-stylized"};
    UIElement* picture = ChildWithClass(*row.Presets->GetHeaderContainer(), "dropdown-header-picture");
    ASSERT_NE(picture, nullptr) << "the closed field shows the selected preset's picture";
    for (int i = 0; i < 4; ++i)
    {
        row.Presets->SetSelectedIndexWithoutNotify(i);
        EXPECT_EQ(row.Presets->GetSelectedLabel(), names[i]);
        EXPECT_TRUE(picture->HasClass(pictures[i])) << names[i];
    }
    row.Presets->SetSelectedIndexWithoutNotify(4);
    EXPECT_EQ(row.Presets->GetSelectedIndex(), -1) << "the list offers only the four presets";
}

// A dial moved away from the preset shows Custom as the field's state, with nothing selected in the list.
TEST(TerrainGrassPresetRow, AChangedDialReadsCustom)
{
    GrassRow row;
    ASSERT_NE(row.Presets, nullptr);
    row.SetBladeHeight(0.5f);
    EXPECT_EQ(row.Grass().Preset, Components::TerrainGrassPreset::Custom);
    EXPECT_EQ(row.Presets->GetSelectedIndex(), -1) << "Custom is a state, not an entry";
    EXPECT_EQ(ShownText(*row.Presets), "Custom");
}

// Picking a preset, from Custom, writes its dials and names it in one undo step, and asks for the dial
// rows to be rebuilt; one undo puts the hand-set dial back.
TEST(TerrainGrassPresetRow, PickingAPresetAppliesItAsOneUndoStep)
{
    GrassRow row;
    ASSERT_NE(row.Presets, nullptr);
    row.SetBladeHeight(0.5f);
    ASSERT_EQ(ShownText(*row.Presets), "Custom");

    row.Presets->SetSelectedIndex(1);
    Components::TerrainGrass lawn{};
    TerrainGrass::ApplyTerrainGrassPreset(lawn, Components::TerrainGrassPreset::Lawn);
    EXPECT_EQ(row.Grass().Preset, Components::TerrainGrassPreset::Lawn);
    EXPECT_FLOAT_EQ(row.Grass().BladeHeight, lawn.BladeHeight);
    EXPECT_FLOAT_EQ(row.Grass().BladesPerSquareMeter, lawn.BladesPerSquareMeter);
    EXPECT_EQ(row.Grass().TipColor, lawn.TipColor);
    EXPECT_EQ(ShownText(*row.Presets), "Lawn");
    EXPECT_EQ(row.Undo.GetUndoCount(), 1u) << "one pick, one undo step";
    EXPECT_EQ(row.Refreshes, 1) << "the dial rows are rebuilt to show the preset's values";

    row.Undo.Undo();
    EXPECT_FLOAT_EQ(row.Grass().BladeHeight, 0.5f);
    EXPECT_EQ(row.Grass().Preset, Components::TerrainGrassPreset::Custom);
    EXPECT_EQ(ShownText(*row.Presets), "Custom");
}

// Every preset's picture has a line in grass-pictures.txt with the look dials it was taken with,
// keyed by the scene file's field names. Each look dial of the preset table is on the line, so a
// change to any of them fails here until the picture is retaken.
TEST(TerrainGrassPresetRow, ThePicturesWereTakenWithThePresetsValues)
{
    const std::filesystem::path record =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Assets/Icons/OptionPictures/grass-pictures.txt";
    std::string text;
    ASSERT_TRUE(ReadFileTextShared(record, text)) << record.generic_string() << " did not read";
    struct Picture
    {
        Components::TerrainGrassPreset Preset;
        const char* Name;
        const char* File;
    };
    const Picture pictures[] = {{Components::TerrainGrassPreset::Meadow, "Meadow", "grass-meadow.png: "},
                                {Components::TerrainGrassPreset::Lawn, "Lawn", "grass-lawn.png: "},
                                {Components::TerrainGrassPreset::Steppe, "Steppe", "grass-steppe.png: "},
                                {Components::TerrainGrassPreset::Stylized, "Stylized", "grass-stylized.png: "}};
    ASSERT_EQ(std::size(pictures), std::size(TerrainGrass::kTerrainGrassPresets)) << "one picture per preset";
    for (const auto dial : TerrainGrass::Detail::kTerrainGrassPresetFloatDials)
        ASSERT_NE(RecordKey(kRecordedFloatDials, dial), nullptr) << "a float look dial has no record key";
    for (const auto dial : TerrainGrass::Detail::kTerrainGrassPresetColorDials)
        ASSERT_NE(RecordKey(kRecordedColorDials, dial), nullptr) << "a color look dial has no record key";
    for (const Picture& picture : pictures)
    {
        const size_t start = text.find(std::string("\n") + picture.File);
        ASSERT_NE(start, std::string::npos) << "no values recorded for " << picture.File;
        const size_t end = text.find('\n', start + 1);
        std::string_view line = std::string_view(text).substr(start + 1, end - start - 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        Components::TerrainGrass preset{};
        TerrainGrass::ApplyTerrainGrassPreset(preset, picture.Preset);
        EXPECT_EQ(RecordValue(line, "preset"), picture.Name) << line;
        ExpectRecordedNumber(line, "gain", 1.0f);
        for (const auto dial : TerrainGrass::Detail::kTerrainGrassPresetColorDials)
        {
            const char* key = RecordKey(kRecordedColorDials, dial);
            EXPECT_EQ(RecordValue(line, key), RecordedColor(preset.*dial)) << key << " in: " << line;
        }
        for (const auto dial : TerrainGrass::Detail::kTerrainGrassPresetFloatDials)
            ExpectRecordedNumber(line, RecordKey(kRecordedFloatDials, dial), preset.*dial);
    }
}
