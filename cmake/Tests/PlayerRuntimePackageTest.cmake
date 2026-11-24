# Verify the staged Player contains the module file the native loader looks up.
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${STAMP_TOOL}" platformdir "${WORK}/platform.txt"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${STAMP_TOOL}" modulefile Eztree "${WORK}/module.txt"
    COMMAND_ERROR_IS_FATAL ANY)
file(STRINGS "${WORK}/platform.txt" _platform LIMIT_COUNT 1)
file(STRINGS "${WORK}/module.txt" _module LIMIT_COUNT 1)
if(_platform STREQUAL "" OR _module STREQUAL "")
    message(FATAL_ERROR "The prebuilt stamp tool did not return an EZTree module path")
endif()
set(_prebuilt "${PACKAGES_DIR}/eztree/Binaries/Runtime/${_platform}/${_module}")
if(NOT EXISTS "${_prebuilt}" OR IS_DIRECTORY "${_prebuilt}")
    message(FATAL_ERROR "The staged Player is missing the EZTree runtime module: ${_prebuilt}")
endif()
