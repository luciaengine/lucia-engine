find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
add_executable(SpriteFramesTests
    SpriteFramesTests.cpp
)
target_compile_features(SpriteFramesTests PUBLIC cxx_std_20)
target_link_libraries(SpriteFramesTests PRIVATE GTest::gtest GTest::gtest_main Sprite2D AssetCore Logger GenerationalVector)
ge_set_output_tests(SpriteFramesTests)
ge_stage_runtime_dependencies(SpriteFramesTests)
gtest_add_tests(TARGET SpriteFramesTests)

add_executable(SpritePlaybackTests
    SpritePlaybackTests.cpp
)
target_compile_features(SpritePlaybackTests PUBLIC cxx_std_20)
target_link_libraries(SpritePlaybackTests PRIVATE GTest::gtest GTest::gtest_main Sprite2D AssetCore Logger GenerationalVector)
ge_set_output_tests(SpritePlaybackTests)
ge_stage_runtime_dependencies(SpritePlaybackTests)
gtest_add_tests(TARGET SpritePlaybackTests)

target_include_directories(SpriteFramesTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Tests)
