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
    find_package(GTest REQUIRED)
    add_executable(MemoryAllocatorTests
        Tests/ArenaImagePinTests.cpp
        Tests/FrameJoinedScopeTests.cpp
        Tests/FrameMemoryTests.cpp
        Tests/ScratchScopeTests.cpp
        Tests/ArenaTests.cpp
        Tests/ArenaUseTests.cpp
        Tests/ObjectPoolTests.cpp
        Tests/ObjectPoolUseTests.cpp
        Tests/RangeAllocatorTests.cpp
        Tests/TrackedBytesTests.cpp
        Source/AllocationHook.cpp
    )
    target_compile_features(MemoryAllocatorTests PRIVATE cxx_std_20)
    target_link_libraries(MemoryAllocatorTests PRIVATE Memory JobSystem Logger GTest::gtest GTest::gtest_main)
    target_include_directories(MemoryAllocatorTests PRIVATE Testing)
    set_source_files_properties(Source/AllocationHook.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    ge_set_output_tests(MemoryAllocatorTests)
    ge_stage_runtime_dependencies(MemoryAllocatorTests)
    add_test(NAME MemoryAllocatorTests COMMAND $<TARGET_FILE:MemoryAllocatorTests>)
endif()
