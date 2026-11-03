add_executable(SceneSubsetCook SceneSubsetCook.cpp)
target_compile_features(SceneSubsetCook PRIVATE cxx_std_20)
target_link_libraries(SceneSubsetCook PRIVATE Engine)
set_target_properties(SceneSubsetCook PROPERTIES FOLDER "Tools")
ge_set_output_tools(SceneSubsetCook)
ge_stage_runtime_dependencies(SceneSubsetCook)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME SceneSubsetCookCliTests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py"
                    --tool $<TARGET_FILE:SceneSubsetCook>)
    endif()
endif()
