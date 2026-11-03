cmake_minimum_required(VERSION 3.20)

project(TileMapECS LANGUAGES CXX)

add_library(TileMapECS OBJECT
    Include/Components/TileMap/TileMapLayerRef.h
    Include/TileMapECS/RegisterTileMap.h
    Include/TileMapECS/TileChunkInstances.h
    Include/TileMapECS/TileMapParallax.h
    Include/TileMapECS/TileMapRenderFeature.h
    Include/TileMapECS/TileMapRuntimeStore.h
    Include/TileMapECS/TileSetGpuTables.h
    Include/Components/TileMap/TileMapCollider.h
    Include/TileMapECS/Systems/TileMapCollisionSystem.h
    Include/TileMapECS/Systems/TileMapExtractionSystem.h
    Source/RegisterTileMap.cpp
    Source/TileChunkInstances.cpp
    Source/TileMapComponents.cpp
    Source/TileMapRenderFeature.cpp
    Source/TileMapRuntimeStore.cpp
    Source/TileSetGpuTables.cpp
    Source/TileSurfaceQuery.cpp
    Source/Systems/TileMapCollisionSystem.cpp
    Source/Systems/TileMapExtractionSystem.cpp
)

target_compile_features(TileMapECS PUBLIC cxx_std_20)

target_include_directories(TileMapECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(TileMapECS
    PUBLIC
        TileMap
        PhysicsECS
        Physics
        GameEngineRendering
        Logger
        ECS
        ECSComponents
        Types
        Mathematics
        AssetCore
        FileSystemCore
)

target_include_directories(TileMapECS PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
)

if(MSVC)
    target_compile_options(TileMapECS PRIVATE /W4 /bigobj)
else()
    target_compile_options(TileMapECS PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
