add_executable(TexturePackageCook TexturePackageCook.cpp)
target_compile_features(TexturePackageCook PRIVATE cxx_std_20)
target_link_libraries(TexturePackageCook PRIVATE Engine)
set_target_properties(TexturePackageCook PROPERTIES FOLDER "Tools")
ge_set_output_tools(TexturePackageCook)
ge_stage_runtime_dependencies(TexturePackageCook)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME TexturePackageCookCliTests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py"
                    --tool $<TARGET_FILE:TexturePackageCook>)
    endif()
endif()
