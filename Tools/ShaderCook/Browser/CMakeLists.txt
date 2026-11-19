cmake_minimum_required(VERSION 3.25)
project(BrowserShaderCompiler LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure the browser shader compiler with emcmake")
endif()

foreach(dependency SHADERC GLSLANG SPIRV_TOOLS SPIRV_HEADERS)
    set(${dependency}_SOURCE_DIR "" CACHE PATH "${dependency} source directory")
    if(NOT EXISTS "${${dependency}_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Set ${dependency}_SOURCE_DIR to its source tree")
    endif()
endforeach()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV-Headers_SOURCE_DIR "${SPIRV_HEADERS_SOURCE_DIR}")
add_subdirectory("${SPIRV_TOOLS_SOURCE_DIR}" spirv-tools EXCLUDE_FROM_ALL)

set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLSLANG_SOURCE_DIR}" glslang EXCLUDE_FROM_ALL)

set(SHADERC_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
set(SHADERC_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
add_subdirectory("${SHADERC_SOURCE_DIR}" shaderc EXCLUDE_FROM_ALL)

set(browserCompilerDirectory "${CMAKE_CURRENT_BINARY_DIR}/compiler")
set(browserToolDirectory "${CMAKE_CURRENT_BINARY_DIR}/compiler-tools")
set(browserToolAssets)
foreach(compiler glslc_exe spirv-opt)
    if(compiler STREQUAL "glslc_exe")
        set(factory GeGlslc)
    else()
        set(factory GeSpirvOpt)
    endif()
    set_target_properties(${compiler} PROPERTIES
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${browserToolDirectory}")
    target_link_options(${compiler} PRIVATE
        "SHELL:-s ENVIRONMENT=web,worker"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=${factory}"
        "SHELL:-s WASM_ASYNC_COMPILATION=0"
        "SHELL:-s FORCE_FILESYSTEM=1"
        "SHELL:-s INVOKE_RUN=0"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=FS,callMain"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s STACK_SIZE=4194304")

    get_target_property(compilerName ${compiler} OUTPUT_NAME)
    if(NOT compilerName)
        set(compilerName "${compiler}")
    endif()
    set(compilerWasm "${browserCompilerDirectory}/${compilerName}.wasm")
    add_custom_command(
        OUTPUT "${compilerWasm}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${browserCompilerDirectory}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${browserToolDirectory}/${compilerName}.wasm" "${browserCompilerDirectory}"
        DEPENDS ${compiler}
        VERBATIM)
    list(APPEND browserToolAssets "${compilerWasm}")
endforeach()

find_program(CARGO_EXECUTABLE cargo REQUIRED)

set(nagaDirectory "${CMAKE_CURRENT_SOURCE_DIR}/naga")
set(nagaBuildDirectory "${CMAKE_CURRENT_BINARY_DIR}/naga")
set(nagaWasm "${browserCompilerDirectory}/naga-browser.wasm")
add_custom_command(
    OUTPUT "${nagaWasm}"
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${nagaBuildDirectory}"
        "${CARGO_EXECUTABLE}" build --manifest-path "${nagaDirectory}/Cargo.toml"
        --locked --release --target wasm32-unknown-unknown
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${browserCompilerDirectory}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${nagaBuildDirectory}/wasm32-unknown-unknown/release/lucia_web_shader_translator.wasm" "${nagaWasm}"
    DEPENDS "${nagaDirectory}/Cargo.toml" "${nagaDirectory}/Cargo.lock" "${nagaDirectory}/src/lib.rs"
    VERBATIM)

set(browserCompilerModules)
foreach(module compiler.mjs spirv.mjs)
    set(destination "${browserCompilerDirectory}/${module}")
    add_custom_command(
        OUTPUT "${destination}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${browserCompilerDirectory}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${module}" "${destination}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${module}"
        VERBATIM)
    list(APPEND browserCompilerModules "${destination}")
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(browserCompilerBundle "${browserCompilerDirectory}/editor-compiler.js")
add_custom_command(
    OUTPUT ${browserCompilerBundle} "${browserCompilerDirectory}/glslc.mjs" "${browserCompilerDirectory}/spirv-opt.mjs"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/stage.py"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${browserToolDirectory}" "${browserCompilerDirectory}"
    DEPENDS glslc_exe spirv-opt compiler.mjs spirv.mjs editor.js stage.py
    VERBATIM)
add_custom_target(BrowserShaderCompilerAssets ALL
    DEPENDS ${browserToolAssets} "${nagaWasm}" ${browserCompilerBundle} ${browserCompilerModules})
