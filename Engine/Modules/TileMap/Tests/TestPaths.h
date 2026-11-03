#pragma once

#include <filesystem>

namespace GameEngine
{
namespace TileMapTests
{

/**
 * Directory the build stages the committed importer fixtures into (next to the
 * test executable — see the POST_BUILD copy in the module CMakeLists), resolved
 * from argv[0] so the suite runs from any working directory.
 */
std::filesystem::path FixtureDir();

} // namespace TileMapTests
} // namespace GameEngine
