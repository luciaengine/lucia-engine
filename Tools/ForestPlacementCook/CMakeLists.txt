find_package(nlohmann_json CONFIG REQUIRED)
add_executable(ForestPlacementCook ForestPlacementCook.cpp ForestOverviewCook.cpp ForestSplatBake.cpp)
target_compile_features(ForestPlacementCook PRIVATE cxx_std_20)
target_link_libraries(ForestPlacementCook PRIVATE Engine nlohmann_json::nlohmann_json)
target_include_directories(ForestPlacementCook PRIVATE $<TARGET_PROPERTY:PageStreaming,INTERFACE_INCLUDE_DIRECTORIES>)
set_target_properties(ForestPlacementCook PROPERTIES FOLDER "Tools")
ge_set_output_tools(ForestPlacementCook)
ge_stage_runtime_dependencies(ForestPlacementCook)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ForestPlacementCookCliTests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py"
                    $<TARGET_FILE:ForestPlacementCook>)
    endif()
endif()
