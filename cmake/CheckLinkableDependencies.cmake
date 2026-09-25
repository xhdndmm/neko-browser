# =============================================================================
# CheckLinkableDependencies — catch "dependency without a library file" at
# configure time instead of as unresolved externals at link time.
#
# An imported target whose IMPORTED_LOCATION / IMPORTED_IMPLIB is missing for
# every configuration contributes *nothing* to the link line and produces no
# configure-time error: the failure only appears later as a wall of
#   LNK2001: unresolved external symbol ...
# linker errors — hard to attribute, and it hides the real cause (a dependency
# found through the wrong search path, triplet or package configuration; see
# the 2026-09 Windows ARM64 release build).
#
# neko_verify_linkable_dependencies() reports, for each dependency target, the
# library file it resolved to, and fails the configure when one resolves to
# nothing.  Keeping the report in the configure log also documents which
# dependency came from where in every CI job.
#
# INTERFACE libraries (e.g. FFmpeg::FFmpeg) hold their link inputs in
# INTERFACE_LINK_LIBRARIES: their referenced targets are checked recursively
# and their plain entries (library files, flags, $<...> expressions) count as
# inputs.
# =============================================================================

# Appends every library-file-ish input of |target| to |out_var|.
function(neko_collect_link_inputs target out_var)
  set(_inputs "")

  if(NOT TARGET ${target})
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()

  get_target_property(_aliased ${target} ALIASED_TARGET)
  if(_aliased)
    # Qt (and other config packages) expose versioned targets through aliases;
    # read through to the real target.
    neko_collect_link_inputs(${_aliased} _inputs)
    set(${out_var} "${_inputs}" PARENT_SCOPE)
    return()
  endif()

  get_target_property(_imported ${target} IMPORTED)
  if(NOT _imported)
    # A library of this project: its artifact is built here, nothing to check.
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()

  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    get_target_property(_deps ${target} INTERFACE_LINK_LIBRARIES)
    set(_collected "")
    foreach(_dep IN LISTS _deps)
      if(TARGET ${_dep})
        neko_collect_link_inputs(${_dep} _dep_inputs)
        list(APPEND _collected ${_dep_inputs})
      else()
        # Plain library file/name, flag, or a generator expression we cannot
        # resolve statically: presence is all we can assert.
        list(APPEND _collected "${_dep}")
      endif()
    endforeach()
    set(${out_var} "${_collected}" PARENT_SCOPE)
    return()
  endif()

  foreach(_property IMPORTED_LOCATION IMPORTED_IMPLIB)
    get_target_property(_value ${target} ${_property})
    if(_value)
      list(APPEND _inputs "${_value}")
    endif()
    get_target_property(_configs ${target} IMPORTED_CONFIGURATIONS)
    foreach(_config IN LISTS _configs)
      get_target_property(_config_value ${target} ${_property}_${_config})
      if(_config_value)
        list(APPEND _inputs "${_config}:${_config_value}")
      endif()
    endforeach()
  endforeach()

  set(${out_var} "${_inputs}" PARENT_SCOPE)
endfunction()

# Reports each dependency's resolved library file; fails when one has none.
function(neko_verify_linkable_dependencies)
  message(STATUS "")
  message(STATUS "Link dependencies (${CMAKE_CURRENT_SOURCE_DIR}):")
  set(_missing "")
  foreach(_target IN LISTS ARGN)
    neko_collect_link_inputs(${_target} _inputs)
    if(_inputs STREQUAL "")
      message(STATUS "  ${_target}: NO LINK INPUT RESOLVED")
      list(APPEND _missing ${_target})
      continue()
    endif()
    list(GET _inputs 0 _first)
    list(LENGTH _inputs _count)
    if(_count GREATER 1)
      message(STATUS "  ${_target}: ${_first} (+${_count} more)")
    else()
      message(STATUS "  ${_target}: ${_first}")
    endif()
  endforeach()

  if(_missing)
    string(REPLACE ";" "\n  - " _report "${_missing}")
    message(FATAL_ERROR
      "Link dependencies with no library file for this configuration:\n"
      "  - ${_report}\n"
      "The build would fail later with unresolved external symbols. Check the "
      "dependency's find module / package configuration and the search paths "
      "(CMAKE_PREFIX_PATH, VCPKG_TARGET_TRIPLET, CMAKE_GENERATOR_PLATFORM).")
  endif()
endfunction()
