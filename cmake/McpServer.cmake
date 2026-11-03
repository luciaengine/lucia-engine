# The checked-in MCP client configuration points at mcp/dist/index.js.
# Register an optional, explicit build target; configure never runs npm.
function(ge_add_mcp_server_target)
    set(mcp_directory "${CMAKE_SOURCE_DIR}/mcp")
    if(NOT EXISTS "${mcp_directory}/package.json")
        return()
    endif()

    # Only PATH availability enables this target. Do not retain a cached
    # executable after npm has been removed from the configure environment.
    find_program(mcp_npm NAMES npm.cmd npm PATHS ENV PATH NO_DEFAULT_PATH NO_CACHE)
    if(NOT mcp_npm)
        message(STATUS "npm not found; optional McpServer target is unavailable (install Node.js >= 20 to enable it)")
        return()
    endif()

    set(dependency_stamp "${mcp_directory}/node_modules/.package-lock.json")
    add_custom_command(
        OUTPUT "${dependency_stamp}"
        COMMAND "${mcp_npm}" ci
        DEPENDS "${mcp_directory}/package.json" "${mcp_directory}/package-lock.json"
        WORKING_DIRECTORY "${mcp_directory}"
        COMMENT "Installing MCP server dependencies"
        VERBATIM)

    file(GLOB_RECURSE mcp_sources CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${mcp_directory}/src/*")
    set(server_entry "${mcp_directory}/dist/index.js")
    add_custom_command(
        OUTPUT "${server_entry}"
        COMMAND "${mcp_npm}" run build
        DEPENDS "${dependency_stamp}" "${mcp_directory}/tsconfig.json" ${mcp_sources}
        WORKING_DIRECTORY "${mcp_directory}"
        COMMENT "Building MCP server"
        VERBATIM)
    add_custom_target(McpServer DEPENDS "${server_entry}")
endfunction()

# The assistant resolves the server through the same content-root rule as other
# staged data: Resources inside a macOS bundle, beside the executable elsewhere.
# A stamp avoids reinstalling dependencies on every incremental Editor build.
function(ge_stage_mcp_server_beside target)
    if(NOT TARGET McpServer)
        return()
    endif()
    find_program(mcp_npm NAMES npm.cmd npm PATHS ENV PATH NO_DEFAULT_PATH NO_CACHE)
    set(mcp_directory "${CMAKE_SOURCE_DIR}/mcp")
    ge_get_staged_content_dir(${target} content_directory)
    set(staged_directory "${content_directory}/mcp")
    set(stage_stamp "${CMAKE_CURRENT_BINARY_DIR}/stage-stamps/mcp-${target}-$<CONFIG>.stamp")
    set(stage_target "Stage${target}McpServer")
    set(sign_commands "")
    if(APPLE AND GE_CODESIGN_MAC_BUNDLE_POST_BUILD)
        list(APPEND sign_commands
            COMMAND ${CMAKE_COMMAND}
                "-DGE_APP_BUNDLE=$<TARGET_BUNDLE_DIR:${target}>"
                "-DGE_APP_EXECUTABLE=$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_NAME:${target}>"
                "-DGE_CODESIGN_IDENTITY=${GE_MAC_CODESIGN_IDENTITY}"
                -P "${CMAKE_SOURCE_DIR}/cmake/CodesignMacAppBundle.cmake")
    endif()
    add_custom_command(OUTPUT "${stage_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/stage-stamps"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${mcp_directory}/dist" "${staged_directory}/dist"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${mcp_directory}/package.json" "${mcp_directory}/package-lock.json" "${staged_directory}"
        COMMAND "${mcp_npm}" ci --prefix "${staged_directory}" --omit=dev --ignore-scripts --no-audit --no-fund
        ${sign_commands}
        COMMAND ${CMAKE_COMMAND} -E touch "${stage_stamp}"
        DEPENDS "${mcp_directory}/dist/index.js" "${mcp_directory}/package.json"
            "${mcp_directory}/package-lock.json" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            "${CMAKE_SOURCE_DIR}/cmake/CodesignMacAppBundle.cmake"
        COMMENT "Staging the MCP server for ${target}"
        VERBATIM)
    add_custom_target(${stage_target} DEPENDS "${stage_stamp}")
    add_dependencies(McpServer ${stage_target})
    if(APPLE AND TARGET StageEditorBundleResources)
        # All resource writers finish before this recipe signs an existing app.
        # The Editor then finishes its own SDK staging and final signature.
        add_dependencies(${stage_target} StageEditorManaged StageEditorAssets StageEditorShaders)
        if(TARGET StageSteamDeckTools)
            add_dependencies(${stage_target} StageSteamDeckTools)
        endif()
        add_dependencies(StageEditorBundleResources ${stage_target})
    endif()
endfunction()
