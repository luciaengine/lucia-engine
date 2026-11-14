cmake_minimum_required(VERSION 3.20)

project(TileMap LANGUAGES CXX)

file(GLOB_RECURSE TILEMAP_PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/Include/TileMap/*.h
)

add_library(TileMap OBJECT
    ${TILEMAP_PUBLIC_HEADERS}
    Source/CollisionEdgeMerge.cpp
    Source/RuleWizard.cpp
    Source/Importers/ImportBudget.h
    Source/Importers/ImportCommon.h
    Source/Importers/LDtkImporter.cpp
    Source/Importers/TiledImporter.cpp
    Source/Importers/TileMapImportResult.cpp
    Source/TerrainMatcher.cpp
    Source/TileAnimation.cpp
    Source/TileCellOrientation.cpp
    Source/TileMapAsset.cpp
    Source/TileMapData.cpp
    Source/TileMapLayerData.cpp
    Source/TileRuleEvaluator.cpp
    Source/TileRules.cpp
    Source/TileSetAsset.cpp
)

set_target_properties(TileMap PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "TileMap"
)

target_include_directories(TileMap
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

find_package(nlohmann_json CONFIG REQUIRED)
# The Tiled importer parses .tmx/.tsx with the same pugixml the UI XML parser
# and MaterialX import use.
find_package(pugixml CONFIG REQUIRED)

target_link_libraries(TileMap
    PUBLIC
        Types
        Mathematics
        AssetCore
        nlohmann_json::nlohmann_json
    PRIVATE
        Logger
        pugixml::pugixml
)

# zlib decodes base64+zlib/gzip Tiled layers, the format Tiled writes by default.
find_package(ZLIB REQUIRED)
target_link_libraries(TileMap PRIVATE ZLIB::ZLIB)

# Self-find GTest (UI/Input pattern): gating on root-scope GTest state instead
# would silently drop the test target whenever the root skips its find.
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(TileMapTests
        Tests/ImportBudgetTests.cpp
        Tests/CollisionEdgeMergeTests.cpp
        Tests/RuleWizardTests.cpp
        Tests/LDtkImporterTests.cpp
        Tests/TerrainMatcherTests.cpp
        Tests/TestMain.cpp
        Tests/TiledImporterTests.cpp
        Tests/TileAnimationTests.cpp
        Tests/TileCellEncodingTests.cpp
        Tests/TileCellOrientationTests.cpp
        Tests/TileMapAssetLoadTests.cpp
        Tests/TileMapAssetTests.cpp
        Tests/TileMapLayerDataTests.cpp
        Tests/TileRuleEvaluatorTests.cpp
        Tests/TileSetAssetLoadTests.cpp
        Tests/TileSetAssetTests.cpp
    )
    target_include_directories(TileMapTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    # TestMain.cpp provides main (fixture-dir resolution from argv[0]), so the
    # target links gtest only, not gtest_main.
    target_link_libraries(TileMapTests PRIVATE
        TileMap
        AssetCore
        Logger
        GTest::gtest
    )
    # Committed importer fixtures, staged next to the test executable.
    add_custom_command(TARGET TileMapTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures"
            "$<TARGET_FILE_DIR:TileMapTests>/TileMapTestFixtures"
        COMMENT "Staging TileMap importer fixtures"
        VERBATIM)
    set_target_properties(TileMapTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests/TileMap"
    )
    ge_set_output_tests(TileMapTests)
    ge_stage_runtime_dependencies(TileMapTests)
    add_test(NAME TileMapTests COMMAND TileMapTests)
endif()
