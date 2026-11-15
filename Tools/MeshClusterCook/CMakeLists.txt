add_executable(MeshClusterCook MeshClusterCook.cpp)
target_compile_features(MeshClusterCook PRIVATE cxx_std_20)
target_link_libraries(MeshClusterCook PRIVATE Engine)
set_target_properties(MeshClusterCook PROPERTIES FOLDER "Tools")
ge_set_output_tools(MeshClusterCook)
ge_stage_runtime_dependencies(MeshClusterCook)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME MeshClusterCookCliTests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py"
                    --tool $<TARGET_FILE:MeshClusterCook>)
    endif()
endif()
