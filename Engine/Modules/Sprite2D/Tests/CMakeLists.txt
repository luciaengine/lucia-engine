find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
add_executable(Sprite2DFramesTests
    SpriteFramesTests.cpp
)
target_compile_features(Sprite2DFramesTests PUBLIC cxx_std_20)
target_link_libraries(Sprite2DFramesTests PRIVATE GTest::gtest GTest::gtest_main Sprite2D AssetCore Logger GenerationalVector)
ge_set_output_tests(Sprite2DFramesTests)
ge_stage_runtime_dependencies(Sprite2DFramesTests)
gtest_add_tests(TARGET Sprite2DFramesTests)

add_executable(SpritePlaybackTests
    SpritePlaybackTests.cpp
)
target_compile_features(SpritePlaybackTests PUBLIC cxx_std_20)
target_link_libraries(SpritePlaybackTests PRIVATE GTest::gtest GTest::gtest_main Sprite2D AssetCore Logger GenerationalVector)
ge_set_output_tests(SpritePlaybackTests)
ge_stage_runtime_dependencies(SpritePlaybackTests)
gtest_add_tests(TARGET SpritePlaybackTests)

target_include_directories(Sprite2DFramesTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Tests)
