set(_ge_rapidraw_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_ge_rapidraw_sources
    "${_ge_rapidraw_dir}/RapidRawDocument.swift"
    "${_ge_rapidraw_dir}/RapidRawRenderer.swift"
    "${_ge_rapidraw_dir}/RapidRawWorkspace.swift"
    "${CMAKE_SOURCE_DIR}/cmake/macos-ports/lucia-media-controls/LuciaMediaControls.swift")
set(_ge_rapidraw_arches "${CMAKE_OSX_ARCHITECTURES}")
if(NOT _ge_rapidraw_arches)
    set(_ge_rapidraw_arches "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(_ge_rapidraw_macos "${CMAKE_OSX_DEPLOYMENT_TARGET}")
if(NOT _ge_rapidraw_macos)
    set(_ge_rapidraw_macos "13.0")
endif()
set(_ge_rapidraw_build "${CMAKE_CURRENT_BINARY_DIR}/RapidRAWHost")
set(_ge_rapidraw_executable "${_ge_rapidraw_build}/RapidRAW")
set(_ge_rapidraw_outputs)
foreach(_ge_arch IN LISTS _ge_rapidraw_arches)
    set(_ge_output "${_ge_rapidraw_build}/RapidRAW-${_ge_arch}")
    add_custom_command(OUTPUT "${_ge_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ge_rapidraw_build}"
        COMMAND /usr/bin/xcrun --sdk macosx swiftc -O -D LUCIA_HOSTED -emit-library
            -target "${_ge_arch}-apple-macos${_ge_rapidraw_macos}"
            -module-cache-path "${_ge_rapidraw_build}/ModuleCache"
            -module-name LuciaRapidRaw ${_ge_rapidraw_sources} -o "${_ge_output}"
        DEPENDS ${_ge_rapidraw_sources}
        VERBATIM)
    list(APPEND _ge_rapidraw_outputs "${_ge_output}")
endforeach()
add_custom_command(OUTPUT "${_ge_rapidraw_executable}"
    COMMAND /usr/bin/lipo -create ${_ge_rapidraw_outputs} -output "${_ge_rapidraw_executable}"
    DEPENDS ${_ge_rapidraw_outputs}
    VERBATIM)
add_custom_target(RapidRawWorkspaceHost DEPENDS "${_ge_rapidraw_executable}")
add_dependencies(Editor RapidRawWorkspaceHost)
set_property(TARGET Editor APPEND PROPERTY LINK_DEPENDS "${_ge_rapidraw_executable};${_ge_rapidraw_dir}/Info.plist")
set(_ge_rapidraw_stage "$<TARGET_FILE_DIR:Editor>/Packages/rapidraw/Host/RapidRAW.app/Contents")
add_custom_command(TARGET Editor POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ge_rapidraw_stage}/MacOS"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_ge_rapidraw_executable}" "${_ge_rapidraw_stage}/MacOS/RapidRAW"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_ge_rapidraw_dir}/Info.plist" "${_ge_rapidraw_stage}/Info.plist"
    COMMENT "Staging the RapidRAW workspace host"
    VERBATIM)

if(BUILD_TESTING)
    set(_ge_rapidraw_tests "${_ge_rapidraw_build}/RapidRawDocumentTests")
    set(_ge_rapidraw_test_sources
        "${_ge_rapidraw_dir}/RapidRawDocument.swift"
        "${_ge_rapidraw_dir}/Tests/RapidRawDocumentTests.swift")
    add_custom_command(OUTPUT "${_ge_rapidraw_tests}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ge_rapidraw_build}"
        COMMAND /usr/bin/xcrun --sdk macosx swiftc
            -module-cache-path "${_ge_rapidraw_build}/ModuleCache"
            ${_ge_rapidraw_test_sources} -o "${_ge_rapidraw_tests}"
        DEPENDS ${_ge_rapidraw_test_sources}
        VERBATIM)
    add_custom_target(RapidRawDocumentTests DEPENDS "${_ge_rapidraw_tests}")
    add_test(NAME RapidRawDocumentTests COMMAND "${_ge_rapidraw_tests}")
endif()
