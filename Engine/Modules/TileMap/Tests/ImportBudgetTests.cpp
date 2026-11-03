#include "Importers/ImportBudget.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace GameEngine::TileMap::Importers;
namespace TestUtils = GameEngine::TestUtils;

TEST(TileImportBudget, ChargesRegularFilesAndDistinguishesMissingSources)
{
    const TestUtils::ScopedTempDir directory{TestUtils::MakeUniqueTempDirectory("tile_source_charge")};
    const auto path = directory.Path() / "source.tmx";
    ImportBudget budget;
    EXPECT_EQ(budget.ReadFile(path), SourceFileCharge::NotARegularFile);
    std::ofstream(path) << "<map/>";
    EXPECT_EQ(budget.ReadFile(path), SourceFileCharge::Charged);
}

TEST(TileImportBudget, RefusesDirectoriesBeforeAReaderCanOpenThem)
{
    const TestUtils::ScopedTempDir directory{TestUtils::MakeUniqueTempDirectory("tile_source_directory")};
    ImportBudget budget;
    EXPECT_EQ(budget.ReadFile(directory.Path()), SourceFileCharge::NotARegularFile);
}

#if !defined(_WIN32)
TEST(TileImportBudget, RefusesAFifoWithoutOpeningIt)
{
    const TestUtils::ScopedTempDir directory{TestUtils::MakeUniqueTempDirectory("tile_source_fifo")};
    const auto path = directory.Path() / "source.tmx";
    ASSERT_EQ(mkfifo(path.c_str(), 0600), 0);
    ImportBudget budget;
    EXPECT_EQ(budget.ReadFile(path), SourceFileCharge::NotARegularFile);
}
#endif

TEST(TileImportBudget, AccumulatesBytesAcrossSourceFiles)
{
    const TestUtils::ScopedTempDir directory{TestUtils::MakeUniqueTempDirectory("tile_source_limit")};
    const auto path = directory.Path() / "large.tmx";
    std::ofstream(path) << "x";
    std::error_code error;
    std::filesystem::resize_file(path, kMaxImportSourceBytes / 2 + 1, error);
    ASSERT_FALSE(error) << error.message();
    ImportBudget budget;
    EXPECT_EQ(budget.ReadFile(path), SourceFileCharge::Charged);
    EXPECT_EQ(budget.ReadFile(path), SourceFileCharge::OverBudget);
}
