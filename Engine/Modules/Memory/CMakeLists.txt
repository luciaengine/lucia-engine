cmake_minimum_required(VERSION 3.20)

project(Memory VERSION 1.0.0 LANGUAGES CXX)

set(MEMORY_SOURCES
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
target_link_libraries(Memory PUBLIC Logger Types)

if(MSVC)
    target_compile_options(Memory PRIVATE /W4)
else()
    target_compile_options(Memory PRIVATE -Wall -Wextra -Wpedantic)
endif()

message(STATUS "Memory: Configured as static library")

