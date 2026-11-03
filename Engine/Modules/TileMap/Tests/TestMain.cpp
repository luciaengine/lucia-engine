#include "TestPaths.h"

#include <gtest/gtest.h>

namespace
{
std::filesystem::path g_ExecutablePath;
} // namespace

namespace GameEngine
{
namespace TileMapTests
{

std::filesystem::path FixtureDir()
{
    return g_ExecutablePath.parent_path() / "TileMapTestFixtures";
}

} // namespace TileMapTests
} // namespace GameEngine

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    std::error_code ec;
    g_ExecutablePath = std::filesystem::absolute(argv[0], ec);
    return RUN_ALL_TESTS();
}
