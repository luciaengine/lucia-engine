# ge_dotnet_versions_newest_first(<list_var>)
#
# Orders a list of .NET version directories (host packs, host/fxr) newest
# first, comparing the version numbers as numbers: 10.0.12 precedes 9.0.20.
# A plain list(SORT) compares text, which puts every 9.x ahead of 10.x.
function(ge_dotnet_versions_newest_first list_var)
  set(_versions ${${list_var}})
  list(SORT _versions COMPARE NATURAL ORDER DESCENDING)
  set(${list_var} ${_versions} PARENT_SCOPE)
endfunction()

# ge_dotnet_install_roots(<out_var> <standard_root>)
#
# The .NET install roots to search for the hosting pack, first match wins: DOTNET_ROOT, the
# folder of the dotnet an earlier configure found (DOTNET_EXECUTABLE, cached), the per-user
# install that dotnet-install.sh makes (~/.dotnet), then <standard_root>. The cached dotnet and
# ~/.dotnet keep the result the same when a reconfigure runs from a shell without DOTNET_ROOT.
function(ge_dotnet_install_roots out_var standard_root)
  set(_roots "")
  if(NOT "$ENV{DOTNET_ROOT}" STREQUAL "")
    list(APPEND _roots "$ENV{DOTNET_ROOT}")
  endif()
  if(DOTNET_EXECUTABLE AND EXISTS "${DOTNET_EXECUTABLE}")
    get_filename_component(_dotnet "${DOTNET_EXECUTABLE}" REALPATH)
    get_filename_component(_dotnet_dir "${_dotnet}" DIRECTORY)
    list(APPEND _roots "${_dotnet_dir}")
  endif()
  if(NOT "$ENV{HOME}" STREQUAL "")
    list(APPEND _roots "$ENV{HOME}/.dotnet")
  endif()
  list(APPEND _roots "${standard_root}")
  list(REMOVE_DUPLICATES _roots)
  set(${out_var} ${_roots} PARENT_SCOPE)
endfunction()
