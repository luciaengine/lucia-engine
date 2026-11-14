if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(LocalizationEditorTests
        Tests/LocalizationEditorModuleTests.cpp
        Editor/LocalizationEditorModuleInit.cpp
        Editor/LocalizationPanel.cpp
        Editor/LocalizationKeyListProvider.cpp)
    target_include_directories(LocalizationEditorTests PRIVATE Editor "${CMAKE_SOURCE_DIR}/Tests")
    target_link_libraries(LocalizationEditorTests PRIVATE
        Engine EditorSDK nlohmann_json::nlohmann_json GTest::gtest_main)
    target_compile_features(LocalizationEditorTests PRIVATE cxx_std_20)
    ge_set_output_tests(LocalizationEditorTests)
    ge_stage_runtime_dependencies(LocalizationEditorTests)
    add_custom_command(TARGET LocalizationEditorTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
            "$<TARGET_FILE_DIR:LocalizationEditorTests>/LocalizationEditorFixtures/localization"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets"
            "$<TARGET_FILE_DIR:LocalizationEditorTests>/LocalizationEditorFixtures/editor")
    # Fixtures are runtime inputs: an asset-only edit must restage them too.
    file(GLOB_RECURSE _localization_test_assets CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/*"
        "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/*")
    set_property(TARGET LocalizationEditorTests APPEND PROPERTY LINK_DEPENDS
        "${_localization_test_assets}")
    add_test(NAME LocalizationEditorTests COMMAND $<TARGET_FILE:LocalizationEditorTests>)
endif()
