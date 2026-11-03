#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine
{
struct BuildSettings;
class BuildPipeline;

/// Host-resolved engine build tool and installed native platform SDKs.
struct MobileExportInputs
{
    std::filesystem::path BuildTool;
    std::filesystem::path EngineRoot;
    std::filesystem::path BuildCache;
    std::filesystem::path HostGlslc;
    std::string ApplicationIdentifier;
    std::filesystem::path AndroidSDK;
    std::filesystem::path AndroidNDK;
    std::string SigningTeam;
    std::string DeviceUDID;
    std::string Version = "1.0";
};

/// Builds the engine-owned Player and packages prepared content without project launchers.
bool RunMobileBuildTool(const BuildSettings& settings, BuildPipeline& pipeline,
                     const MobileExportInputs& inputs, const std::filesystem::path& contentRoot,
                     const std::function<void(const std::string&)>& onLine, std::string& error);
}
