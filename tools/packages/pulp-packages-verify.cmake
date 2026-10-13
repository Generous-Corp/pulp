find_package(Git QUIET)

# Each package below is fetched at the full commit recorded in
# packages.lock.json and checked against the lock's integrity before it is
# added to the build. integrity = "sha256-" + sha256 of
#   git -c core.autocrlf=false -c core.eol=lf -c tar.umask=0002 \
#       archive --format=tar <commit>
# A mismatch stops the configure. FETCHCONTENT_FULLY_DISCONNECTED and
# FETCHCONTENT_SOURCE_DIR_<NAME> are honoured by FetchContent itself; the
# check needs no network.
function(_pulp_package_verify name commit integrity)
  string(TOLOWER "${name}" _lc)
  FetchContent_GetProperties(${name})
  set(_src "${${_lc}_SOURCE_DIR}")
  set(_stamp "${commit} ${integrity} ${_src}")
  if("${_PULP_PACKAGE_VERIFIED_${_lc}}" STREQUAL "${_stamp}")
    return()
  endif()
  if(NOT EXISTS "${_src}/.git")
    message(WARNING "Pulp package ${name}: ${_src} is not a git checkout, so "
      "its locked integrity (${integrity}) cannot be verified. This is "
      "expected only for an explicit source override.")
    return()
  endif()
  if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "Pulp package ${name}: git is required to verify "
      "the integrity recorded in packages.lock.json")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_src}" rev-parse --verify --quiet "${commit}^{commit}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _resolved
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _rc EQUAL 0 OR NOT "${_resolved}" STREQUAL "${commit}")
    message(FATAL_ERROR "Pulp package ${name}: locked commit ${commit} is "
      "not present in ${_src}")
  endif()
  set(_dir "${CMAKE_BINARY_DIR}/pulp-packages")
  file(MAKE_DIRECTORY "${_dir}")
  set(_tar "${_dir}/${_lc}-${commit}.tar")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_src}" -c core.autocrlf=false -c core.eol=lf
            -c tar.umask=0002 archive --format=tar -o "${_tar}" "${commit}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Pulp package ${name}: git archive failed: ${_err}")
  endif()
  file(SHA256 "${_tar}" _digest)
  file(REMOVE "${_tar}")
  if(NOT "sha256-${_digest}" STREQUAL "${integrity}")
    message(FATAL_ERROR "Pulp package ${name}: integrity mismatch for ${commit}\n"
      "  locked:   ${integrity}\n"
      "  computed: sha256-${_digest}\n"
      "Refusing to build an unverified dependency. If the pin changed on "
      "purpose, run 'pulp update --apply' to re-pin it.")
  endif()
  set(_PULP_PACKAGE_VERIFIED_${_lc} "${_stamp}" CACHE INTERNAL
    "Verified Pulp package pin for ${name}")
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
