find_package(Git QUIET)

# Each package below is fetched at the full commit recorded in
# packages.lock.json and checked before it is added to the build:
#   1. the checkout's HEAD is the locked commit,
#   2. its working tree has no modified, staged or untracked files,
#   3. integrity == "sha256-" + sha256 of the bytes printed by
#        git ls-tree -r -z --full-tree <commit>
#      (computed from tree objects only, so no .gitattributes entry, checkout
#      filter, git configuration or clone depth changes it).
# Any failure stops the configure. A FETCHCONTENT_SOURCE_DIR_<NAME> set by the
# developer is the one exception: the same checks run, but a failure is a
# warning naming the override. The checks need no network, so they hold under
# FETCHCONTENT_FULLY_DISCONNECTED.
function(_pulp_package_verify name commit integrity)
  string(TOLOWER "${name}" _lc)
  string(TOUPPER "${name}" _uc)
  FetchContent_GetProperties(${name})
  set(_src "${${_lc}_SOURCE_DIR}")
  set(_override "")
  if(NOT "${FETCHCONTENT_SOURCE_DIR_${_uc}}" STREQUAL ""
     AND NOT PULP_FETCHCONTENT_SOURCE_IS_SHARED_${_uc})
    set(_override "FETCHCONTENT_SOURCE_DIR_${_uc}=${FETCHCONTENT_SOURCE_DIR_${_uc}}")
    message(WARNING "Pulp package ${name}: using the source override "
      "${_override} instead of the commit locked in packages.lock.json "
      "(${commit}). A failed check below is reported, not enforced.")
  endif()
  if(_override STREQUAL "")
    set(_level FATAL_ERROR)
  else()
    set(_level WARNING)
  endif()
  set(_fix "Delete ${_src} (or the build directory) to re-fetch the locked commit, or run 'pulp update --apply' if the pin changed on purpose.")

  if(NOT EXISTS "${_src}/.git")
    message(${_level} "Pulp package ${name}: ${_src} is not a git checkout, "
      "so it cannot be checked against packages.lock.json (${commit}, "
      "${integrity}). ${_fix}")
    return()
  endif()
  if(NOT GIT_EXECUTABLE)
    message(${_level} "Pulp package ${name}: git is required to verify the "
      "pin recorded in packages.lock.json")
    return()
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_src}" rev-parse --verify --quiet "HEAD^{commit}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _head
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _rc EQUAL 0 OR NOT "${_head}" STREQUAL "${commit}")
    message(${_level} "Pulp package ${name}: ${_src} is checked out at "
      "'${_head}', but packages.lock.json pins ${commit}. ${_fix}")
    return()
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_src}" status --porcelain=v1
            --untracked-files=all --ignore-submodules=none
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _dirty ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(${_level} "Pulp package ${name}: git status failed in ${_src}: ${_err}")
    return()
  endif()
  if(NOT "${_dirty}" STREQUAL "")
    message(${_level} "Pulp package ${name}: ${_src} has local changes or "
      "untracked files, so the build would not be the locked commit:\n"
      "${_dirty}${_fix}")
    return()
  endif()

  set(_dir "${CMAKE_BINARY_DIR}/pulp-packages")
  file(MAKE_DIRECTORY "${_dir}")
  set(_listing "${_dir}/${_lc}-${commit}.tree")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_src}" ls-tree -r -z --full-tree "${commit}"
    OUTPUT_FILE "${_listing}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(${_level} "Pulp package ${name}: git ls-tree failed: ${_err}")
    return()
  endif()
  file(SHA256 "${_listing}" _digest)
  file(REMOVE "${_listing}")
  if(NOT "sha256-${_digest}" STREQUAL "${integrity}")
    message(${_level} "Pulp package ${name}: integrity mismatch for ${commit}\n"
      "  locked:   ${integrity}\n"
      "  computed: sha256-${_digest}\n"
      "Refusing to build an unverified dependency. ${_fix}")
  endif()
endfunction()

# Populate (the declared SOURCE_SUBDIR never exists, so FetchContent does not
# add the package yet), verify, then add the package's own CMake project.
macro(_pulp_package_make_available name commit integrity add_subdir)
  FetchContent_MakeAvailable(${name})
  _pulp_package_verify(${name} ${commit} ${integrity})
  string(TOLOWER "${name}" _pulp_package_lc)
  if(${add_subdir} AND EXISTS "${${_pulp_package_lc}_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${${_pulp_package_lc}_SOURCE_DIR}"
                     "${${_pulp_package_lc}_BINARY_DIR}")
  endif()
endmacro()
