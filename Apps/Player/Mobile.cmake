include(${CMAKE_SOURCE_DIR}/cmake/DesktopPlayerSources.cmake)
ge_read_desktop_player_sources("${CMAKE_CURRENT_SOURCE_DIR}" sources)
list(TRANSFORM sources PREPEND "Source/")
find_package(SDL3 CONFIG REQUIRED)
list(APPEND sources Source/PlayerDemoControls.cpp Source/PlayerDemoControls.h)
if(ANDROID)
    add_library(Player SHARED ${sources})
    set_target_properties(Player PROPERTIES OUTPUT_NAME main)
else()
    set(GE_MOBILE_APP_IDENTIFIER "org.openengine.player" CACHE STRING "Mobile application identifier")
    add_executable(Player MACOSX_BUNDLE ${sources})
    set_target_properties(Player PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "${GE_MOBILE_APP_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_NAME "Player"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${GE_MOBILE_APP_IDENTIFIER}")
endif()
target_include_directories(Player PRIVATE Source)
target_compile_features(Player PRIVATE cxx_std_20)
target_compile_definitions(Player PRIVATE GE_PLAYER_MOVIE_RECORDER=0)
target_link_libraries(Player PRIVATE Engine SDL3::SDL3)
if(ANDROID)
    target_link_libraries(Player PRIVATE log android)
endif()
set_target_properties(Player PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mobile" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mobile")

# The mobile Player has one Engine copy; native game registration lives in the executable.
target_include_directories(Player PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Include)
if(GE_MOBILE_GAME_SOURCES)
    include("${GE_MOBILE_GAME_SOURCES}")
endif()
