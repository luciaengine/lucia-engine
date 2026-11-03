#pragma once

#include <cstdint>

namespace GameEngine::HierarchyCommandIds
{

// Built-in Hierarchy context menu commands (must fit in 16-bit on Win32).
inline constexpr std::uint32_t kCmdCreateEmptyEntity = 0x4101;
inline constexpr std::uint32_t kCmdCreateAsEmptyParent = 0x4102;
inline constexpr std::uint32_t kCmdCreateShapeCube = 0x4110;
inline constexpr std::uint32_t kCmdCreateShapeSphere = 0x4111;
inline constexpr std::uint32_t kCmdCreateShapeCapsule = 0x4112;
inline constexpr std::uint32_t kCmdCreateShapePlane = 0x4113;
inline constexpr std::uint32_t kCmdCreateCamera = 0x4120;
inline constexpr std::uint32_t kCmdCreateLightDirectional = 0x4130;
inline constexpr std::uint32_t kCmdCreateLightPoint = 0x4131;
inline constexpr std::uint32_t kCmdCreateLightSpot = 0x4132;
inline constexpr std::uint32_t kCmdCreateLightAmbient = 0x4133;
inline constexpr std::uint32_t kCmdCreateLightArea = 0x4134;
inline constexpr std::uint32_t kCmdCreateTerrain = 0x4140;      // "Terrain (512 m)" preset + the create_terrain IPC default
inline constexpr std::uint32_t kCmdCreateTerrainLarge = 0x4147; // "Terrain - Large (4 km)"
inline constexpr std::uint32_t kCmdCreatePlanet5km = 0x4148;    // "Planet (5 km)"
inline constexpr std::uint32_t kCmdCreatePlanet50km = 0x4149;   // "Planet (50 km)"
inline constexpr std::uint32_t kCmdCreatePostProcessVolume = 0x4141;
inline constexpr std::uint32_t kCmdCreateOcean = 0x4143;
inline constexpr std::uint32_t kCmdCreateParticleEmitter = 0x4142;
inline constexpr std::uint32_t kCmdCreateWindVolume = 0x4144;
inline constexpr std::uint32_t kCmdCreateReflectionProbe = 0x4145;
inline constexpr std::uint32_t kCmdCreateSkyEnvironment = 0x4146;
inline constexpr std::uint32_t kCmdCreateDDGIVolume = 0x414A;
inline constexpr std::uint32_t kCmdNewScene = 0x4150;
inline constexpr std::uint32_t kCmdDuplicate = 0x4160;
inline constexpr std::uint32_t kCmdSortCustom = 0x4170;
inline constexpr std::uint32_t kCmdSortAlphabetical = 0x4171;
inline constexpr std::uint32_t kCmdSortType = 0x4172;
inline constexpr std::uint32_t kCmdSortAscending = 0x4173;
inline constexpr std::uint32_t kCmdSortDescending = 0x4174;
inline constexpr std::uint32_t kCmdShowRenderLayer = 0x4175;
inline constexpr std::uint32_t kCmdShowVcsSceneDiffDots = 0x4176;
inline constexpr std::uint32_t kCmdShowSortingLayer = 0x4177;
inline constexpr std::uint32_t kCmdShowMeshLocation = 0x4180;
inline constexpr std::uint32_t kCmdAddToBookmarks = 0x4190;
inline constexpr std::uint32_t kCmdLoadRecentSceneBase = 0x41A0;
inline constexpr std::uint32_t kCmdLoadRecentSceneCount = 10;
inline constexpr std::uint32_t kCmdLoadRecentSceneEmpty = 0x41AF;

} // namespace GameEngine::HierarchyCommandIds
