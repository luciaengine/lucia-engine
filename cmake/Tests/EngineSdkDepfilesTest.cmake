# Checks the SDK the Editor build staged, run by ctest as
#   cmake -DSDK_DIR=<editor dir>/SDK -P <this file>
# A depfile lists the paths of the tree that built it, so the SDK must not hold one.
if(NOT DEFINED SDK_DIR OR NOT IS_DIRECTORY "${SDK_DIR}")
    message(FATAL_ERROR "EngineSdkDepfilesTest: SDK_DIR must name the staged SDK directory")
endif()

# Files only: the bundle's vulkan/icd.d is a directory of loader manifests.
file(GLOB_RECURSE _depfiles LIST_DIRECTORIES false RELATIVE "${SDK_DIR}" "${SDK_DIR}/*.d")
if(_depfiles)
    list(JOIN _depfiles "\n  " _listing)
    message(FATAL_ERROR "EngineSdkDepfilesTest: the staged SDK holds build depfiles:\n  ${_listing}")
endif()
message(STATUS "EngineSdkDepfilesTest: passed")
