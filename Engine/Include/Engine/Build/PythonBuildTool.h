#pragma once
#include <filesystem>
namespace GameEngine
{
/// Finds Python for installed build tools, including package-manager paths on macOS.
std::filesystem::path FindPythonBuildExecutable();
}
