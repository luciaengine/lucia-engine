cmake_minimum_required(VERSION 3.20)

project(Memory VERSION 1.0.0 LANGUAGES CXX)

set(MEMORY_SOURCES
    Source/Arena.cpp
    Source/FrameJoinedScope.cpp
    Source/FrameMemory.cpp
    Source/ScratchScope.cpp
    Source/ObjectPool.cpp
    Source/RangeAllocator.cpp
    Source/StatsRegistry.cpp
    Source/TrackedBytes.cpp
    Source/AllocationCategory.cpp
    Source/AllocationCounter.cpp
)

add_library(Memory OBJECT ${MEMORY_SOURCES})
set_target_properties(Memory PROPERTIES INTERFACE_GE_ALLOCATION_COUNTER_LINKED ON)
set_property(TARGET Memory APPEND PROPERTY COMPATIBLE_INTERFACE_BOOL GE_ALLOCATION_COUNTER_LINKED)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 17.0)
    target_sources(Memory PRIVATE Source/MemoryResource_iOS.cpp)
endif()

target_compile_features(Memory PUBLIC cxx_std_20)

# Use existing public headers from Engine/Include
# so includes like "Memory/AllocationCategory.h" continue to work.
target_include_directories(Memory
    PUBLIC
        ${CMAKE_SOURCE_DIR}/Engine/Include
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        Source
        Testing
)

# Dependencies
target_link_libraries(Memory PUBLIC Types)

if(MSVC)
    target_compile_options(Memory PRIVATE /W4)
else()
    target_compile_options(Memory PRIVATE -Wall -Wextra -Wpedantic)
endif()

message(STATUS "Memory: Configured as static library")


if(BUILD_TESTING AND NOT EMSCRIPTEN)
    add_subdirectory(Tests)
endif()

if(NOT EMSCRIPTEN)
    add_subdirectory(Bench)
endif()
