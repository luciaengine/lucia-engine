#include <gtest/gtest.h>
#include "Components/Rendering/WindVolume.h"
#include "Components/Transform.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/WindVolumeResolver.h"

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
namespace
{
ECS::EntityHandle AddWind(ECS::World& world, Components::WindVolume wind,
                          Components::WorldTransform transform = {})
{
    const auto entity = world.CreateEntity();
    world.AddComponentImmediate(entity, wind);
    world.AddComponentImmediate(entity, transform);
    return entity;
}
}

TEST(WeatherWind, RotatedLocalVolumeBlendsRainAtItsBoundaryAndPacksTheSameBounds)
{
    ECS::World world;
    Components::WindVolume wind{};
    wind.DirectionX = 1.0f;
    wind.DirectionZ = 0.0f;
    wind.Speed = 4.0f;
    wind.RainAmount = 0.0f;
    wind.BlendDistance = 2.0f;
    Components::WorldTransform transform{};
    // Rotate the local X extent onto world Z, and translate to x=10.
    transform.Basis[0].x = 0.0f;
    transform.Basis[0].z = 4.0f;
    transform.Basis[2].x = -2.0f;
    transform.Basis[2].z = 0.0f;
    transform.Basis[1].y = 6.0f;
    transform.Position.X = 10.0f;
    AddWind(world, wind, transform);
    EXPECT_FLOAT_EQ(WindVolumeResolver::ResolveAt(world, 10, 0, 0).RainAmount, 0.0f);
    const auto boundary = WindVolumeResolver::ResolveAt(world, 12, 0, 0);
    EXPECT_FLOAT_EQ(boundary.RainAmount, 0.5f);
    EXPECT_FLOAT_EQ(boundary.VelocityX, 2.0f);
    EXPECT_FLOAT_EQ(WindVolumeResolver::ResolveAt(world, 14, 0, 0).RainAmount, 1.0f);
    const auto gpu = WindVolumeResolver::ExtractForGpu(world);
    ASSERT_FLOAT_EQ(gpu.Count[0], 1.0f);
    const auto& v = gpu.Volumes[0];
    EXPECT_FLOAT_EQ(v.CenterBlend[0], 10.0f);
    EXPECT_FLOAT_EQ(v.CenterBlend[3], 2.0f);
    EXPECT_FLOAT_EQ(v.AxisXExtent[2], 1.0f);
    EXPECT_FLOAT_EQ(v.AxisXExtent[3], 2.0f);
    EXPECT_FLOAT_EQ(v.AxisYExtent[3], 3.0f);
    EXPECT_FLOAT_EQ(v.AxisZExtent[0], -1.0f);
    EXPECT_FLOAT_EQ(v.AxisZExtent[3], 1.0f);
    EXPECT_FLOAT_EQ(v.VelocityWeight[0], 4.0f);
    EXPECT_FLOAT_EQ(v.Mode[2], 0.0f);
}

TEST(WeatherWind, PriorityAdditiveAmountAndDisabledVolumesMatchGpuSubmission)
{
    ECS::World world;
    Components::WindVolume wind{};
    wind.IsGlobal = true;
    wind.Priority = 0;
    wind.RainAmount = 0.5f;
    wind.LayerMask = 1;
    const auto first = AddWind(world, wind);
    wind.Priority = 10;
    wind.RainAmount = 1.5f;
    wind.Weight = 0.5f;
    wind.BlendMode = Components::WindVolumeBlendMode::Additive;
    AddWind(world, wind);
    wind.Priority = 20;
    wind.RainAmount = 0.0f;
    wind.Weight = 1.0f;
    const auto disabled = AddWind(world, wind);
    world.AddComponentImmediate(disabled, ECS::Disabled{});
    EXPECT_FLOAT_EQ(WindVolumeResolver::ResolveAt(world, 100, 0, 0).RainAmount, 0.75f);
    const auto gpu = WindVolumeResolver::ExtractForGpu(world, 1);
    ASSERT_FLOAT_EQ(gpu.Count[0], 2.0f);
    EXPECT_FLOAT_EQ(gpu.Volumes[0].Mode[2], 0.5f);
    EXPECT_FLOAT_EQ(gpu.Volumes[1].Mode[2], 1.5f);
    EXPECT_FLOAT_EQ(gpu.Volumes[1].VelocityWeight[3], 0.5f);
    EXPECT_FLOAT_EQ(WindVolumeResolver::ExtractForGpu(world, 2).Count[0], 0.0f);
    world.RemoveComponentImmediate<Components::WindVolume>(first);
    EXPECT_FLOAT_EQ(WindVolumeResolver::ExtractForGpu(world).Count[0], 1.0f);
}

TEST(WeatherWind, GpuCapacityRetainsHighestPrioritiesAndClearsEmptyWorld)
{
    ECS::World world;
    Components::WindVolume wind{};
    wind.IsGlobal = true;
    wind.DirectionX = 1.0f;
    wind.DirectionZ = 0.0f;
    for (uint32 i = 0; i < kMaxWeatherWindVolumes + 3; ++i)
    {
        wind.Priority = static_cast<int32>(i);
        wind.Speed = static_cast<float>(i);
        AddWind(world, wind);
    }
    const auto gpu = WindVolumeResolver::ExtractForGpu(world);
    ASSERT_FLOAT_EQ(gpu.Count[0], static_cast<float>(kMaxWeatherWindVolumes));
    EXPECT_FLOAT_EQ(gpu.Volumes[0].VelocityWeight[0], 3.0f);
    EXPECT_FLOAT_EQ(gpu.Volumes[kMaxWeatherWindVolumes - 1].VelocityWeight[0],
                    static_cast<float>(kMaxWeatherWindVolumes + 2));
    ECS::World empty;
    EXPECT_FLOAT_EQ(WindVolumeResolver::ExtractForGpu(empty).Count[0], 0.0f);
    EXPECT_FLOAT_EQ(WindVolumeResolver::ResolveAt(empty, 0, 0, 0).RainAmount, 1.0f);
}
