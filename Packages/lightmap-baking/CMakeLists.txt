# The baker is owned by the opt-in editor package, never linked into Engine or Player.
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(ShadowLightmapBakerTests
        Tests/ShadowLightmapBakerTests.cpp
        Tests/LightmapSurfaceComposeTests.cpp
        Editor/Baker/ShadowLightmapBaker.cpp
        Editor/Baker/LightmapRayTracing.cpp)
    target_include_directories(ShadowLightmapBakerTests PRIVATE
        Editor "${CMAKE_SOURCE_DIR}/Engine/Tests")
    target_link_libraries(ShadowLightmapBakerTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    target_compile_features(ShadowLightmapBakerTests PRIVATE cxx_std_20)
    ge_set_output_tests(ShadowLightmapBakerTests)
    ge_stage_runtime_dependencies(ShadowLightmapBakerTests)
    add_dependencies(ShadowLightmapBakerTests StageTestAssets)
    add_test(NAME ShadowLightmapBakerTests COMMAND $<TARGET_FILE:ShadowLightmapBakerTests>)
    add_custom_command(TARGET ShadowLightmapBakerTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
            "$<TARGET_FILE_DIR:ShadowLightmapBakerTests>/LightmapTestAssets")
endif()
