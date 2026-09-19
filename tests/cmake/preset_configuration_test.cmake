# =============================================================================
# Regression test for the build / test presets' "configuration" field,
# driven by ctest as build_system.preset_configuration.
#
# Visual Studio (and Ninja Multi-Config) are *multi-configuration* generators:
# `cmake --build --preset release` without an explicit configuration builds the
# generator's default configuration -- Debug -- while `ctest --preset release`
# looks for Release binaries in the per-configuration output directory.  A
# preset that omits the field therefore ships Debug binaries from a release
# workflow: Windows CI built `build/release/bin/Debug/...` that way.
#
# Every visible build / test preset must name the configuration its configure
# preset selects through CMAKE_BUILD_TYPE.
# =============================================================================

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED NEKO_PRESETS_FILE)
  message(FATAL_ERROR "NEKO_PRESETS_FILE is required")
endif()

file(READ "${NEKO_PRESETS_FILE}" _neko_json)

# Number of entries in a top-level preset section ("buildPresets", ...).
function(neko_preset_count section out_var)
  string(JSON _count ERROR_VARIABLE _error LENGTH "${_neko_json}" "${section}")
  if(_error)
    message(FATAL_ERROR "cannot read preset section '${section}': ${_error}")
  endif()
  set(${out_var} "${_count}" PARENT_SCOPE)
endfunction()

# CMAKE_BUILD_TYPE selected by the configure preset named |configure_name|.
# Leaves the result empty when that preset does not name a build type (a
# configure preset may leave the configuration list to the generator).  The
# section is a JSON array, so the entry is found by name.
function(neko_configured_build_type configure_name out_var)
  string(JSON _count ERROR_VARIABLE _error LENGTH "${_neko_json}" "configurePresets")
  if(_error)
    message(FATAL_ERROR "cannot read configurePresets: ${_error}")
  endif()
  math(EXPR _last "${_count} - 1")
  foreach(_index RANGE 0 ${_last})
    string(JSON _preset_name GET "${_neko_json}" "configurePresets" ${_index} "name")
    if(_preset_name STREQUAL configure_name)
      string(JSON _type ERROR_VARIABLE _error
        GET "${_neko_json}" "configurePresets" ${_index} "cacheVariables" "CMAKE_BUILD_TYPE")
      if(NOT _error)
        set(${out_var} "${_type}" PARENT_SCOPE)
      endif()
      return()
    endif()
  endforeach()
endfunction()

# Fails when a visible preset omits "configuration" or names one that differs
# from the CMAKE_BUILD_TYPE of the configure preset it is attached to.  The
# section is a JSON array, so presets are addressed by index.
function(neko_check_preset section index out_checked)
  string(JSON _name ERROR_VARIABLE _error GET "${_neko_json}" "${section}" ${index} "name")
  if(_error)
    message(FATAL_ERROR "cannot read ${section}[${index}]: ${_error}")
  endif()

  string(JSON _hidden ERROR_VARIABLE _error GET "${_neko_json}" "${section}" ${index} "hidden")
  # string(JSON) reports a JSON boolean as ON / OFF.
  if(NOT _error AND _hidden STREQUAL "ON")
    return()
  endif()

  string(JSON _configuration ERROR_VARIABLE _error
    GET "${_neko_json}" "${section}" ${index} "configuration")
  if(_error)
    message(FATAL_ERROR
      "${section} preset '${_name}' does not set \"configuration\": on a "
      "multi-configuration generator cmake would build the default (Debug) "
      "configuration instead of the one the preset name promises.")
  endif()

  string(JSON _configure ERROR_VARIABLE _error
    GET "${_neko_json}" "${section}" ${index} "configurePreset")
  if(_error)
    return() # Nothing to compare against.
  endif()

  neko_configured_build_type("${_configure}" _build_type)
  if(NOT _build_type)
    return() # Nothing the configuration could disagree with.
  endif()

  if(NOT _configuration STREQUAL _build_type)
    message(FATAL_ERROR
      "${section} preset '${_name}' builds configuration '${_configuration}' "
      "but configure preset '${_configure}' selects '${_build_type}'.")
  endif()
  set(${out_checked} TRUE PARENT_SCOPE)
endfunction()

set(_checked 0)
foreach(_section IN ITEMS buildPresets testPresets)
  neko_preset_count("${_section}" _count)
  math(EXPR _last "${_count} - 1")
  foreach(_index RANGE 0 ${_last})
    set(_visible FALSE)
    neko_check_preset("${_section}" "${_index}" _visible)
    if(_visible)
      math(EXPR _checked "${_checked} + 1")
    endif()
  endforeach()
endforeach()

if(_checked EQUAL 0)
  message(FATAL_ERROR "no visible build or test preset was checked")
endif()

message(STATUS "preset configurations: ${_checked} preset(s) checked, OK")
