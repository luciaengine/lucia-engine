# Link settings that an imported target adds to a wasm link line.
#
# emcc takes the last "-s NAME=value" on a link line. A setting that an imported
# target carries comes after the settings of the build, so it replaces them, and
# the link gives no message.

# ge_wasm_drop_link_setting(<imported target> <setting name>)
#
# Removes each INTERFACE_LINK_OPTIONS entry of the target that sets
# <setting name>. A setting with a longer name stays: STACK_SIZE does not match
# ASYNCIFY_STACK_SIZE.
function(ge_wasm_drop_link_setting target setting)
    get_target_property(_options ${target} INTERFACE_LINK_OPTIONS)
    if(NOT _options)
        return()
    endif()
    list(FILTER _options EXCLUDE REGEX "-s ?${setting}(=|$)")
    set_target_properties(${target} PROPERTIES INTERFACE_LINK_OPTIONS "${_options}")
endfunction()

# ge_wasm_require_no_link_setting(<imported target> <setting name>)
#
# Stops the configure if the target sets <setting name>. Call it where a target
# of the build links the imported target.
function(ge_wasm_require_no_link_setting target setting)
    get_target_property(_found ${target} INTERFACE_LINK_OPTIONS)
    if(NOT _found)
        return()
    endif()
    list(FILTER _found INCLUDE REGEX "-s ?${setting}(=|$)")
    if(_found)
        message(FATAL_ERROR
            "${target} carries the link setting \"${_found}\".\n"
            "emcc takes the last -s on a link line, so this value replaces the "
            "${setting} of the build in each executable that links ${target}.\n"
            "Fix: call ge_wasm_drop_link_setting(${target} ${setting}) at the root "
            "scope, after the packages are installed and before the first "
            "subdirectory that links ${target}.")
    endif()
endfunction()
