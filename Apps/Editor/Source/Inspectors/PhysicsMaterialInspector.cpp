#include "Inspectors/PhysicsMaterialInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "AssetCore/Asset.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "PhysicsECS/PhysicsMaterialAsset.h"

#include "UI/Controls/EnumField.h"

#include <fstream>
#include <memory>
#include <string>

namespace GameEngine
{
namespace
{
using Physics::MaterialCombine;
using PhysicsECS::PhysicsMaterialAsset;

constexpr EnumEntry<MaterialCombine> kCombineEntries[] = {
    {MaterialCombine::Average, "Average"},
    {MaterialCombine::Minimum, "Minimum"},
    {MaterialCombine::Multiply, "Multiply"},
    {MaterialCombine::Maximum, "Maximum"},
};

// The one place the combine semantics are spelled out for the author. Kept
// identical between the two rows: the rule is the same, only the value differs.
constexpr const char* kCombineTooltip =
    "How this surface's value meets the other surface's: Average = (a + b) / 2, "
    "Minimum = min(a, b), Multiply = a x b, Maximum = max(a, b). When two materials ask for "
    "different modes the stronger one wins, in the order Average, Minimum, Multiply, Maximum — "
    "so a Maximum-restitution trampoline stays bouncy against an Average-restitution crate.";
constexpr const char* kFrictionTooltip =
    "Resistance to sliding. 0 is frictionless; 0.2 to 1.0 is the usual range.";
constexpr const char* kRestitutionTooltip = "Bounciness. 0 absorbs the impact, 1 is fully elastic.";

constexpr float32 kMinFriction = 0.0f;
constexpr float32 kMaxFriction = 2.0f;
constexpr float32 kMinRestitution = 0.0f;
constexpr float32 kMaxRestitution = 1.0f;

void SaveToDisk(PhysicsMaterialAsset& asset)
{
    Vector<uint8> bytes;
    if (!asset.SaveToData(bytes))
    {
        Logger::Log::Error("PhysicsMaterialInspector: failed to serialize '{}'",
                           asset.GetPath().string());
        return;
    }

    const auto temporaryPath = FileSystem::MakeTemporarySiblingPath(asset.GetPath());
    std::ofstream out(temporaryPath, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out)
    {
        std::error_code error;
        std::filesystem::remove(temporaryPath, error);
        Logger::Log::Error("PhysicsMaterialInspector: cannot write '{}'",
                           asset.GetPath().string());
        return;
    }
    if (!FileSystem::PublishFile(temporaryPath, asset.GetPath()))
        Logger::Log::Error("PhysicsMaterialInspector: cannot publish '{}'",
                           asset.GetPath().string());
}

void BuildPhysicsMaterialInspector(const InspectorContext& ctx, PhysicsMaterialAsset& asset)
{
    InspectorUI::AddTextBlock(ctx.Parent, asset.GetPath().string(), "inspector-asset-path");

    InspectorDrag::AddFloatRowWithDrag(
        ctx.Parent, "Friction", asset.Material().friction,
        [&asset](float v) { asset.Material().friction = v; },
        [&asset](float v)
        {
            asset.Material().friction = v;
            SaveToDisk(asset);
        },
        Physics::PhysicsMaterial{}.friction, kFrictionTooltip, kMinFriction, kMaxFriction);

    InspectorDrag::AddFloatRowWithDrag(
        ctx.Parent, "Restitution", asset.Material().restitution,
        [&asset](float v) { asset.Material().restitution = v; },
        [&asset](float v)
        {
            asset.Material().restitution = v;
            SaveToDisk(asset);
        },
        Physics::PhysicsMaterial{}.restitution, kRestitutionTooltip, kMinRestitution,
        kMaxRestitution);

    InspectorUI::AddEnumRow(ctx.Parent, "Friction Combine", kCombineEntries,
                              asset.Material().frictionCombine, kCombineTooltip)
        ->SetOnValueChanged(
            [&asset](MaterialCombine v)
            {
                asset.Material().frictionCombine = v;
                SaveToDisk(asset);
            });

    InspectorUI::AddEnumRow(ctx.Parent, "Restitution Combine", kCombineEntries,
                              asset.Material().restitutionCombine, kCombineTooltip)
        ->SetOnValueChanged(
            [&asset](MaterialCombine v)
            {
                asset.Material().restitutionCombine = v;
                SaveToDisk(asset);
            });

    if (ctx.ShowInfoCards)
        InspectorUI::AddInfoCard(ctx.Parent, kCombineTooltip);
}

} // namespace

void RegisterPhysicsMaterialInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(
        AssetType::PhysicsMaterial,
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.Object)
                return;
            auto* asset = dynamic_cast<PhysicsMaterialAsset*>(static_cast<Asset*>(ctx.Object));
            if (!asset)
                return;
            BuildPhysicsMaterialInspector(ctx, *asset);
        });
}

} // namespace GameEngine
