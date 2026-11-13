# Contract test for the SDK's generated GameEngineConfig.cmake, run by ctest as
#   cmake -DSDK=<staged SDK dir> -DGENERATOR=<generator> -DWORK=<scratch> -P <this file>
# Covered: the Player an export compiles (the SDK's own template and sources)
# configures and links against the staged SDK with GAMEENGINE_VCPKG_INSTALLED
# unset, as an editor outside its build tree exports it. The vcpkg headers come
# from the SDK's staged vcpkg-include; Engine.dll carries every vcpkg dependency.
foreach(_required SDK GENERATOR WORK)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "EngineSdkPlayerLinkTest.cmake: SDK, GENERATOR and WORK are required")
    endif()
endforeach()

if(NOT EXISTS "${SDK}/templates/Player/GeneratedCMakeLists.txt.in")
    message(FATAL_ERROR "EngineSdkPlayerLinkTest: no Player template in '${SDK}/templates/Player'; build the Editor first")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/src")

# On Linux the Player links against a copy of the SDK whose libEngine.so has no RUNPATH. A build tree's
# or a package's RUNPATH would resolve libEngine.so's dependencies and hide a missing -rpath-link. Links
# in WORK, the copy's parent, stand in for the Editor folder that -rpath-link names.
set(_sdk "${SDK}")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(_sdk "${WORK}/sdk")
    file(COPY "${SDK}/" DESTINATION "${_sdk}")
    file(GLOB _engine_libraries "${_sdk}/lib/*/libEngine.so")
    if(NOT _engine_libraries)
        message(FATAL_ERROR "EngineSdkPlayerLinkTest: no libEngine.so under '${SDK}/lib'")
    endif()
    foreach(_library IN LISTS _engine_libraries)
        file(RPATH_REMOVE FILE "${_library}")
        file(READ_ELF "${_library}" RPATH _rpath RUNPATH _runpath)
        if(NOT "${_rpath}${_runpath}" STREQUAL "")
            message(FATAL_ERROR "EngineSdkPlayerLinkTest: '${_library}' still searches '${_rpath}${_runpath}'")
        endif()
    endforeach()
    get_filename_component(_editor_dir "${SDK}/.." ABSOLUTE)
    file(GLOB _editor_libraries "${_editor_dir}/*.so" "${_editor_dir}/*.so.*")
    foreach(_library IN LISTS _editor_libraries)
        get_filename_component(_name "${_library}" NAME)
        file(CREATE_LINK "${_library}" "${WORK}/${_name}" SYMBOLIC)
    endforeach()
endif()

set(_template_dir "${_sdk}/templates/Player")
file(GLOB _template_sources "${_template_dir}/*.cpp" "${_template_dir}/*.h")
file(COPY ${_template_sources} DESTINATION "${WORK}/src")

set(GE_GAME_NAME "SdkLinkProbe")
set(GE_SDK_CMAKE_DIR "${_sdk}/cmake")
file(GLOB _player_sources "${WORK}/src/*.cpp")
list(JOIN _player_sources "\n    " GE_DISCOVERED_SOURCES)
set(GE_USER_INCLUDE_DIRS "")
set(GE_APP_ICON_SECTION "")
configure_file("${_template_dir}/GeneratedCMakeLists.txt.in" "${WORK}/CMakeLists.txt" @ONLY)

# Release, the configuration a shipped game builds in; the SDK picks the staged
# libs of the matching CRT flavor.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${WORK}" -B "${WORK}/build" -G "${GENERATOR}"
            "-DGameEngine_DIR=${_sdk}/cmake" -DGAMEENGINE_BUILD_CONFIG=Release
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_output)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR "EngineSdkPlayerLinkTest: the Player did not configure without "
                        "GAMEENGINE_VCPKG_INSTALLED:\n${_configure_output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${WORK}/build" --config Release
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_output)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR "EngineSdkPlayerLinkTest: the Player did not link without "
                        "GAMEENGINE_VCPKG_INSTALLED:\n${_build_output}")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    file(READ_ELF "${WORK}/build/Player" RPATH _rpath RUNPATH _runpath)
    if(NOT "${_rpath}${_runpath}" STREQUAL "\$ORIGIN")
        message(FATAL_ERROR "EngineSdkPlayerLinkTest: the Player searches '${_rpath}${_runpath}' for its "
                            "libraries; a packaged game carries them beside the executable (\$ORIGIN)")
    endif()
endif()

file(REMOVE_RECURSE "${WORK}")
message(STATUS "EngineSdkPlayerLinkTest: passed")
