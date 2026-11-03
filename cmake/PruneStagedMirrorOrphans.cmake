# Prune orphans from a staged mirror of a source directory. A mirror is a
# copy_directory_if_different of SRC into DST; that copy never deletes, so a
# file deleted from SRC lives on in DST and suites keep passing against assets
# a fresh tree does not have (the dead-IBL class: >250 MB of deleted assets
# living on in staged trees).
#
# The mirror owns exactly what it staged. DST is often a composite: the
# exe-anchored Assets/ next to a test executable is shared by every test in
# bin/<Config>/Tests, the Editor's asset root merges a dozen source trees with
# build products, and other targets stage their own files into both. Deleting
# whatever DST has that SRC lacks would delete those files whenever this mirror
# runs after them, so the prune instead records SRC's file list at each staging
# in a manifest and removes only files that were in the previous record and are
# no longer in SRC. Files it never staged — sibling content, build products,
# runtime state written into the tree — are never candidates, and several
# mirrors into one DST each prune only their own record.
#
# The manifest names the (SRC, DST) pair, so mirrors that share a DST, and
# mirrors that share a manifest directory, never share a record. It lives
# beside DST as <DST>.<key>.mirror-manifest, or — when DST is what ships or is
# mirrored onward — under MANIFEST_DIR as <DST name>.<key>.mirror-manifest.
# Args: -DSRC=<dir> -DDST=<dir> [-DMANIFEST_DIR=<dir>]
# SRC_ROOTS is the union exported into a fully generated destination. Unlike
# composite asset mirrors, that destination contains no independently owned files.
# FULLY_MANAGED_DST also prunes files from an older export with no manifest.
# EXCLUDE_TOP_LEVEL_DIRS names source subtrees deliberately omitted from that export.
if(NOT DST)
    message(FATAL_ERROR "PruneStagedMirrorOrphans.cmake: DST must not be empty")
endif()
if(DEFINED SRC_ROOTS)
    if(NOT FULLY_MANAGED_DST)
        message(FATAL_ERROR "PruneStagedMirrorOrphans.cmake: SRC_ROOTS requires FULLY_MANAGED_DST")
    endif()
    set(_source_roots "${SRC_ROOTS}")
else()
    if(NOT SRC)
        message(FATAL_ERROR "PruneStagedMirrorOrphans.cmake: SRC must not be empty")
    endif()
    get_filename_component(SRC "${SRC}" ABSOLUTE)
    set(_source_roots "${SRC}")
endif()
if(DEFINED MANIFEST_DIR AND NOT MANIFEST_DIR)
    message(FATAL_ERROR "PruneStagedMirrorOrphans.cmake: MANIFEST_DIR must not be empty when given")
endif()

get_filename_component(DST "${DST}" ABSOLUTE)
string(SHA1 _key "${_source_roots}\n${DST}")
string(SUBSTRING "${_key}" 0 8 _key)
if(DEFINED MANIFEST_DIR)
    get_filename_component(MANIFEST_DIR "${MANIFEST_DIR}" ABSOLUTE)
    get_filename_component(_dst_name "${DST}" NAME)
    set(_manifest "${MANIFEST_DIR}/${_dst_name}.${_key}.mirror-manifest")
else()
    set(_manifest "${DST}.${_key}.mirror-manifest")
endif()

set(_current "")
foreach(_source IN LISTS _source_roots)
    if(IS_DIRECTORY "${_source}")
        if(FULLY_MANAGED_DST)
            file(GLOB_RECURSE _files FOLLOW_SYMLINKS LIST_DIRECTORIES false RELATIVE "${_source}" "${_source}/*")
        else()
            file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${_source}" "${_source}/*")
        endif()
        list(APPEND _current ${_files})
    endif()
endforeach()
foreach(_excluded IN LISTS EXCLUDE_TOP_LEVEL_DIRS)
    list(FILTER _current EXCLUDE REGEX "^${_excluded}(/|$)")
endforeach()
list(REMOVE_DUPLICATES _current)
list(SORT _current)

set(_previous "")
if(FULLY_MANAGED_DST)
    file(GLOB_RECURSE _previous LIST_DIRECTORIES false RELATIVE "${DST}" "${DST}/*")
elseif(EXISTS "${_manifest}")
    file(STRINGS "${_manifest}" _previous ENCODING UTF-8)
endif()

set(_orphans ${_previous})
if(_orphans AND _current)
    list(REMOVE_ITEM _orphans ${_current})
endif()

# Orphans that could not be removed stay recorded so the next staging retries
# them; forgetting one would leave it staged for good.
set(_retained "")
foreach(_rel IN LISTS _orphans)
    # A case-only rename on a case-insensitive filesystem lists under the new
    # spelling while the old one still resolves; that is a rename, not an orphan.
    string(REGEX REPLACE "/.*$" "" _top_level "${_rel}")
    set(_source_exists FALSE)
    list(FIND EXCLUDE_TOP_LEVEL_DIRS "${_top_level}" _excluded_index)
    if(_excluded_index EQUAL -1)
        foreach(_source IN LISTS _source_roots)
            if(EXISTS "${_source}/${_rel}")
                if(NOT FULLY_MANAGED_DST OR NOT IS_DIRECTORY "${_source}/${_rel}")
                    set(_source_exists TRUE)
                    break()
                endif()
            endif()
        endforeach()
    endif()
    if(_source_exists)
        continue()
    endif()
    file(REMOVE "${DST}/${_rel}")
    # file(REMOVE) fails silently on a file the process may not delete (a handle
    # the running editor holds, a read-only directory).
    if(EXISTS "${DST}/${_rel}")
        if(FULLY_MANAGED_DST)
            message(FATAL_ERROR "PruneStagedMirrorOrphans: generated export still contains obsolete '${DST}/${_rel}'")
        endif()
        message(WARNING "PruneStagedMirrorOrphans: could not remove ${DST}/${_rel}; retrying at the next staging")
        list(APPEND _retained "${_rel}")
        continue()
    endif()
    # Sweep the directories this removal emptied, innermost first, stopping at
    # the first one that still has content and never above DST itself.
    get_filename_component(_dir "${DST}/${_rel}" DIRECTORY)
    while(NOT _dir STREQUAL DST AND IS_DIRECTORY "${_dir}")
        file(GLOB _remaining "${_dir}/*")
        if(_remaining)
            break()
        endif()
        file(REMOVE_RECURSE "${_dir}")
        get_filename_component(_dir "${_dir}" DIRECTORY)
    endwhile()
endforeach()

# Fully managed exports have no per-writer record to preserve in the shipped tree.
if(FULLY_MANAGED_DST)
    return()
endif()

set(_record ${_current} ${_retained})
list(SORT _record)
if(NOT "${_previous}" STREQUAL "${_record}")
    string(JOIN "\n" _content ${_record})
    file(WRITE "${_manifest}" "${_content}\n")
endif()
