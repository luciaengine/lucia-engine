# Exercises cmake/WasmLinkSettings.cmake against scratch projects that hold an
# imported target with the link settings of the KTX package. No toolchain.
#
#   cmake -DSCRIPT=<WasmLinkSettings.cmake> -DWORK=<scratch dir> -P WasmLinkSettingsTest.cmake
cmake_minimum_required(VERSION 3.25)
foreach(_required SCRIPT WORK)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "WasmLinkSettingsTest: ${_required} is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${WORK}")

set(_package_settings
    "SHELL:-s ALLOW_MEMORY_GROWTH=1;SHELL:-s MALLOC=emmalloc;SHELL:-s ASYNCIFY_STACK_SIZE=1024")

# configure_probe(<case> <stack setting entry> <commands> <out rc> <out normalized output>)
# The probe writes the link options of the imported target to <case>/options.txt.
function(configure_probe case stackSetting commands rcVar textVar)
    set(_source "${WORK}/${case}")
    file(MAKE_DIRECTORY "${_source}")
    file(WRITE "${_source}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.25)\n"
        "project(WasmLinkSettingsProbe LANGUAGES NONE)\n"
        "include(\"${SCRIPT}\")\n"
        "add_library(KTX::ktx INTERFACE IMPORTED)\n"
        "set_target_properties(KTX::ktx PROPERTIES INTERFACE_LINK_OPTIONS \"${_package_settings};${stackSetting}\")\n"
        "${commands}\n"
        "get_target_property(_options KTX::ktx INTERFACE_LINK_OPTIONS)\n"
        "file(WRITE \"${_source}/options.txt\" \"\${_options}\")\n")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S "${_source}" -B "${_source}/build"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    # CMake indents and breaks the lines of a message(); collapse all
    # whitespace so an assertion on the text cannot fail on formatting.
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${rcVar} "${_rc}" PARENT_SCOPE)
    set(${textVar} "${_text}" PARENT_SCOPE)
endfunction()

# A consumer that sees the setting stops the configure and states the fix.
configure_probe(kept "SHELL:-s STACK_SIZE=96kb"
    "ge_wasm_require_no_link_setting(KTX::ktx STACK_SIZE)" _rc _text)
if(_rc EQUAL 0)
    message(FATAL_ERROR "kept: the configure accepted a target that sets STACK_SIZE: ${_text}")
endif()
foreach(_needle IN ITEMS "STACK_SIZE=96kb" "ge_wasm_drop_link_setting(KTX::ktx STACK_SIZE)")
    string(FIND "${_text}" "${_needle}" _at)
    if(_at LESS 0)
        message(FATAL_ERROR "kept: the refusal lacks \"${_needle}\": ${_text}")
    endif()
endforeach()

# After the drop, the consumer accepts the target. The other settings stay,
# which includes the setting whose name only ends in STACK_SIZE.
foreach(_form IN ITEMS "SHELL:-s STACK_SIZE=96kb" "-sSTACK_SIZE=96kb")
    string(MD5 _case "${_form}")
    configure_probe(dropped-${_case} "${_form}"
        "ge_wasm_drop_link_setting(KTX::ktx STACK_SIZE)\nge_wasm_require_no_link_setting(KTX::ktx STACK_SIZE)"
        _rc _text)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "dropped (${_form}): the configure failed: ${_text}")
    endif()
    file(READ "${WORK}/dropped-${_case}/options.txt" _options)
    if(NOT _options STREQUAL _package_settings)
        message(FATAL_ERROR "dropped (${_form}): the remaining settings are \"${_options}\"")
    endif()
endforeach()

# A target without the property is accepted and stays without it.
configure_probe(none "" "set_property(TARGET KTX::ktx PROPERTY INTERFACE_LINK_OPTIONS)
ge_wasm_drop_link_setting(KTX::ktx STACK_SIZE)
ge_wasm_require_no_link_setting(KTX::ktx STACK_SIZE)
get_property(_isSet TARGET KTX::ktx PROPERTY INTERFACE_LINK_OPTIONS SET)
if(_isSet)
    message(FATAL_ERROR \"the functions set link options on a target that had none\")
endif()" _rc _text)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "none: the configure failed: ${_text}")
endif()

message(STATUS "WasmLinkSettingsTest: passed")
