cmake_minimum_required(VERSION 3.20)

project(PageStreaming LANGUAGES CXX)

# Page streaming: the .gepage store of a terrain field (a pyramid of fixed-size pages), its
# cooker, reader and asynchronous page loader, the generated provider of noise and flat bases,
# and the shipped terrain container. Device-free: the GPU half (physical caches, page tables, the
# resolve) lives with the terrain renderer in TerrainECS and CBTTerrain.
add_library(PageStreaming OBJECT
    Source/Bc5PageCodec.cpp
    Source/BytePageCodec.cpp
    Source/GeneratedHeightPageProvider.cpp
    Source/Height/HeightCookBands.cpp
    Source/Height/HeightPageCodec.cpp
    Source/Height/HeightPageCooker.cpp
    Source/Height/HeightSourceRows.cpp
    Source/Height/HeightStepPlanner.cpp
    Source/Height/HeightStoreCache.cpp
    Source/Height/HeightStoreKey.cpp
    Source/HeightPageOverlay.cpp
    Source/NonInheritedFile.cpp
    Source/PageAddress.cpp
    Source/PageCache.cpp
    Source/PageCodec.cpp
    Source/PageFaceSeams.cpp
    Source/PageFieldFilter.cpp
    Source/PageLoader.cpp
    Source/PagePyramidBuilder.cpp
    Source/PageResidency.cpp
    Source/PageStore/PageStoreFormat.cpp
    Source/PageStore/PageStorePatcher.cpp
    Source/PageStore/PageStoreReader.cpp
    Source/PageStore/PageStoreSerialization.cpp
    Source/PageStore/PageStoreWriter.cpp
    Source/PlanetPageCooker.cpp
    Source/SourceRangeReader.cpp
    Source/TerrainPageContainer.cpp
)

target_compile_features(PageStreaming PUBLIC cxx_std_20)
set_target_properties(PageStreaming PROPERTIES FOLDER "Engine/Modules")

target_include_directories(PageStreaming
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(PageStreaming
    PUBLIC
        Types
        AssetCore
        Mathematics
    PRIVATE
        # The loader's reader threads (Assets/AssetIOService.h); objects link once into Engine.
        AssetRuntime
        JobSystem
        FileSystem
        FileWatcher
        Terrain
        Logger
)

# stb_image for 16-bit PNG heightmaps; stb_image_impl.cpp is compiled by Engine.
find_path(STB_INCLUDE_DIRS "stb_image.h")
target_include_directories(PageStreaming PRIVATE ${STB_INCLUDE_DIRS})
