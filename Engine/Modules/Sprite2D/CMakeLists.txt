cmake_minimum_required(VERSION 3.20)
project(Sprite2D LANGUAGES CXX)

add_library(Sprite2D OBJECT
    Include/Sprite2D/SpriteFrames.h
    Include/Sprite2D/SpritePlayback.h
    Source/SpriteFrames.cpp
    Source/SpritePlayback.cpp
)
set_target_properties(Sprite2D PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "2D")
target_include_directories(Sprite2D PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(Sprite2D PUBLIC Types AssetCore nlohmann_json::nlohmann_json PRIVATE Logger Serialization)
if(BUILD_TESTING)
    add_subdirectory(Tests)
endif()
