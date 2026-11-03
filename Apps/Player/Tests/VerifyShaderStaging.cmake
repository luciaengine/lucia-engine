# Shader builds are only useful when the runtime and export template consume
# the produced bytes. Check actual artifacts, including both CBT heap arms.
foreach(_required COMPILED_SHADER_DIR ENGINE_MODULES_DIR PLAYER_SHADER_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "VerifyShaderStaging: ${_required} is required")
    endif()
endforeach()

function(require_matching_file source staged)
    if(NOT EXISTS "${source}" OR NOT EXISTS "${staged}")
        message(FATAL_ERROR "Missing shader staging input/output: '${source}' -> '${staged}'")
    endif()
    file(SHA256 "${source}" _source_hash)
    file(SHA256 "${staged}" _staged_hash)
    if(NOT _source_hash STREQUAL _staged_hash)
        message(FATAL_ERROR "Stale staged shader: '${staged}' differs from '${source}'")
    endif()
endfunction()

file(GLOB _compiled_shaders
    "${COMPILED_SHADER_DIR}/*.spv" "${COMPILED_SHADER_DIR}/*.shaderpkg")
if(NOT _compiled_shaders)
    message(FATAL_ERROR "No compiled shaders; build the Player before checking its payload")
endif()
set(_destinations "${PLAYER_SHADER_DIR}")
if(DEFINED TEMPLATE_SHADER_DIR)
    list(APPEND _destinations "${TEMPLATE_SHADER_DIR}")
endif()
foreach(_destination IN LISTS _destinations)
    foreach(_source IN LISTS _compiled_shaders)
        get_filename_component(_name "${_source}" NAME)
        require_matching_file("${_source}" "${_destination}/${_name}")
    endforeach()
    foreach(_kernel cbt_kernels.comp.spv cbt_kernels_heap32.comp.spv)
        require_matching_file("${COMPILED_SHADER_DIR}/${_kernel}" "${_destination}/CBT/${_kernel}")
    endforeach()
    # ShaderComposer reads these for runtime material variants, even when no
    # fixed shader package depends on a particular authored surface.
    foreach(_mapping
        "Rendering/Shaders/Adapters|Adapters"
        "Rendering/Shaders/Includes|Includes"
        "Rendering/Shaders/Surfaces|Surfaces"
        "Rendering/Shaders/Particles|Particles"
        "GPUFogParticles/Shaders/Surfaces|GPUFogParticles/Surfaces"
        "TerrainGrass/Shaders/TerrainGrass|TerrainGrass"
        "Rendering/Shaders/Ocean|Ocean")
        string(REPLACE "|" ";" _parts "${_mapping}")
        list(GET _parts 0 _source_relative)
        list(GET _parts 1 _staged_relative)
        set(_source_root "${ENGINE_MODULES_DIR}/${_source_relative}")
        file(GLOB_RECURSE _sources LIST_DIRECTORIES false RELATIVE "${_source_root}" "${_source_root}/*")
        if(NOT _sources)
            message(FATAL_ERROR "No runtime shader sources under '${_source_root}'")
        endif()
        foreach(_relative IN LISTS _sources)
            require_matching_file("${_source_root}/${_relative}"
                                  "${_destination}/${_staged_relative}/${_relative}")
        endforeach()
    endforeach()
endforeach()
list(LENGTH _compiled_shaders _count)
message(STATUS "Player shader payload matches ${_count} compiled shaders, both CBT heaps and runtime sources")
