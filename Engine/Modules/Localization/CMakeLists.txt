cmake_minimum_required(VERSION 3.20)
project(Localization LANGUAGES CXX)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc i18n data)
add_library(Localization OBJECT Source/LocalizationCatalog.cpp Source/LocalizationAsset.cpp Source/LocalizationDocument.cpp Source/LocalizationPo.cpp Source/MessageCheck.cpp)
target_include_directories(Localization PUBLIC Include)
target_compile_features(Localization PUBLIC cxx_std_20)
set_target_properties(Localization PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(Localization PUBLIC Events Types AssetCore PRIVATE Logger nlohmann_json::nlohmann_json ICU::uc ICU::i18n ICU::data)
# ICU's data is linked through ICU::data; retain all upstream notices in exports.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty/ICU/LICENSE.txt"
    DESTINATION licenses/icu)
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(LocalizationDocumentTests Tests/LocalizationDocumentTests.cpp)
    target_link_libraries(LocalizationDocumentTests PRIVATE Engine GTest::gtest_main)
    ge_set_output_tests(LocalizationDocumentTests)
    ge_stage_runtime_dependencies(LocalizationDocumentTests)
    add_test(NAME LocalizationDocumentTests COMMAND LocalizationDocumentTests)
    set_tests_properties(LocalizationDocumentTests PROPERTIES LABELS "Localization;Unit" TIMEOUT 60)
    add_executable(LocalizationTests Tests/LocalizationCatalogTests.cpp)
    target_link_libraries(LocalizationTests PRIVATE Engine GTest::gtest_main)
    ge_set_output_tests(LocalizationTests)
    ge_stage_runtime_dependencies(LocalizationTests)
    add_custom_command(TARGET LocalizationTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LocalizationTests>/licenses/icu"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty/ICU/LICENSE.txt"
            "$<TARGET_FILE_DIR:LocalizationTests>/licenses/icu/LICENSE.txt")
    add_custom_command(TARGET LocalizationTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LocalizationTests>/Localization"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Examples/Resources/menu.localization"
            "$<TARGET_FILE_DIR:LocalizationTests>/Localization/menu.localization")
    add_test(NAME LocalizationTests COMMAND LocalizationTests)
    set_tests_properties(LocalizationTests PROPERTIES LABELS "Localization;Unit" TIMEOUT 60)
endif()
