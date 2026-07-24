# Distributed under the OSI-approved BSD 3-Clause License. See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.
#
# Derived from Catch2's extras/CatchAddTests.cmake.
#
# Pulp override:
# - On Windows, Catch2 per-case discovery breaks when a discovered test name
#   contains non-ASCII punctuation and CTest later passes that name back as a
#   filter argument.
# - For those executables only, register one CTest entry for the whole binary
#   instead of one entry per discovered case.

function(add_command NAME)
  set(_args "")
  # use ARGV* instead of ARGN, because ARGN splits arrays into multiple arguments
  math(EXPR _last_arg ${ARGC}-1)
  foreach(_n RANGE 1 ${_last_arg})
    set(_arg "${ARGV${_n}}")
    if(_arg MATCHES "[^-./:a-zA-Z0-9_]")
      set(_args "${_args} [==[${_arg}]==]")
    else()
      set(_args "${_args} ${_arg}")
    endif()
  endforeach()
  set(script "${script}${NAME}(${_args})\n" PARENT_SCOPE)
endfunction()

function(catch_discover_tests_impl)

  cmake_parse_arguments(
    ""
    ""
    "TEST_TARGET;TEST_EXECUTABLE;TEST_WORKING_DIR;TEST_OUTPUT_DIR;TEST_OUTPUT_PREFIX;TEST_OUTPUT_SUFFIX;TEST_PREFIX;TEST_REPORTER;TEST_SPEC;TEST_SUFFIX;TEST_LIST;CTEST_FILE"
    "TEST_EXTRA_ARGS;TEST_PROPERTIES;TEST_LABELS;TEST_EXECUTOR;TEST_DL_PATHS;TEST_DL_FRAMEWORK_PATHS"
    ${ARGN}
  )

  set(prefix "${_TEST_PREFIX}")
  set(suffix "${_TEST_SUFFIX}")
  set(spec ${_TEST_SPEC})
  set(extra_args ${_TEST_EXTRA_ARGS})
  set(properties ${_TEST_PROPERTIES})
  set(labels ${_TEST_LABELS})
  set(reporter ${_TEST_REPORTER})
  set(output_dir ${_TEST_OUTPUT_DIR})
  set(output_prefix ${_TEST_OUTPUT_PREFIX})
  set(output_suffix ${_TEST_OUTPUT_SUFFIX})
  set(dl_paths ${_TEST_DL_PATHS})
  set(dl_framework_paths ${_TEST_DL_FRAMEWORK_PATHS})
  set(environment_modifications "")
  set(script)
  set(suite)
  set(tests)

  if(WIN32)
    set(dl_paths_variable_name PATH)
  elseif(APPLE)
    set(dl_paths_variable_name DYLD_LIBRARY_PATH)
  else()
    set(dl_paths_variable_name LD_LIBRARY_PATH)
  endif()

  if(NOT EXISTS "${_TEST_EXECUTABLE}")
    message(FATAL_ERROR
      "Specified test executable '${_TEST_EXECUTABLE}' does not exist"
    )
  endif()

  if(dl_paths)
    cmake_path(CONVERT "$ENV{${dl_paths_variable_name}}" TO_NATIVE_PATH_LIST env_dl_paths)
    list(PREPEND env_dl_paths "${dl_paths}")
    cmake_path(CONVERT "${env_dl_paths}" TO_NATIVE_PATH_LIST paths)
    set(ENV{${dl_paths_variable_name}} "${paths}")
  endif()

  if(APPLE AND dl_framework_paths)
    cmake_path(CONVERT "$ENV{DYLD_FRAMEWORK_PATH}" TO_NATIVE_PATH_LIST env_dl_framework_paths)
    list(PREPEND env_dl_framework_paths "${dl_framework_paths}")
    cmake_path(CONVERT "${env_dl_framework_paths}" TO_NATIVE_PATH_LIST paths)
    set(ENV{DYLD_FRAMEWORK_PATH} "${paths}")
  endif()

  execute_process(
    COMMAND ${_TEST_EXECUTOR} "${_TEST_EXECUTABLE}" ${spec} --list-tests --verbosity quiet
    OUTPUT_VARIABLE output
    RESULT_VARIABLE result
    WORKING_DIRECTORY "${_TEST_WORKING_DIR}"
  )
  if(NOT ${result} EQUAL 0)
    message(FATAL_ERROR
      "Error running test executable '${_TEST_EXECUTABLE}':\n"
      "  Result: ${result}\n"
      "  Output: ${output}\n"
    )
  endif()

  if(reporter)
    set(reporter_arg "--reporter ${reporter}")

    execute_process(
      COMMAND ${_TEST_EXECUTOR} "${_TEST_EXECUTABLE}" ${spec} ${reporter_arg} --list-reporters
      OUTPUT_VARIABLE reporter_check_output
      RESULT_VARIABLE reporter_check_result
      WORKING_DIRECTORY "${_TEST_WORKING_DIR}"
    )
    if(${reporter_check_result} EQUAL 255)
      message(FATAL_ERROR
        "\"${reporter}\" is not a valid reporter!\n"
      )
    elseif(NOT ${reporter_check_result} EQUAL 0)
      message(FATAL_ERROR
        "Error running test executable '${_TEST_EXECUTABLE}':\n"
        "  Result: ${reporter_check_result}\n"
        "  Output: ${reporter_check_output}\n"
      )
    endif()
  endif()

  if(output_dir AND NOT IS_ABSOLUTE ${output_dir})
    set(output_dir "${_TEST_WORKING_DIR}/${output_dir}")
    if(NOT EXISTS ${output_dir})
      file(MAKE_DIRECTORY ${output_dir})
    endif()
  endif()

  if(dl_paths)
    foreach(path ${dl_paths})
      cmake_path(NATIVE_PATH path native_path)
      list(PREPEND environment_modifications "${dl_paths_variable_name}=path_list_prepend:${native_path}")
    endforeach()
  endif()

  if(APPLE AND dl_framework_paths)
    foreach(path ${dl_framework_paths})
      cmake_path(NATIVE_PATH path native_path)
      list(PREPEND environment_modifications "DYLD_FRAMEWORK_PATH=path_list_prepend:${native_path}")
    endforeach()
  endif()

  string(REPLACE "\r\n" "\n" output "${output}")
  string(REPLACE "\r" "\n" output "${output}")
  string(REPLACE ";" "\;" output_list "${output}")
  string(REPLACE "\n" ";" output_list "${output_list}")

  set(first_test "")
  foreach(line ${output_list})
    if(NOT line STREQUAL "")
      set(first_test "${line}")
      break()
    endif()
  endforeach()

  set(use_windows_binary_fallback OFF)
  if(WIN32 AND NOT output STREQUAL "")
    if(output MATCHES "[^ -~\n]")
      set(use_windows_binary_fallback ON)
    endif()
  endif()

  if(use_windows_binary_fallback)
    if(_TEST_TARGET)
      set(fallback_label "${_TEST_TARGET}")
    elseif(first_test)
      set(fallback_label "${first_test}")
    else()
      get_filename_component(fallback_label "${_TEST_EXECUTABLE}" NAME_WE)
    endif()

    string(REGEX REPLACE "[^A-Za-z0-9_ .:+-]" "_" fallback_label "${fallback_label}")
    if(first_test)
      string(REGEX REPLACE "[^A-Za-z0-9_ .:+-]" "_" first_test_clean "${first_test}")
      set(fallback_name "${prefix}${fallback_label}: ${first_test_clean}${suffix}")
    else()
      set(fallback_name "${prefix}${fallback_label}${suffix}")
    endif()

    if(output_dir)
      string(REGEX REPLACE "[^A-Za-z0-9_]" "_" fallback_output_name "${fallback_label}")
      set(output_dir_arg "--out ${output_dir}/${output_prefix}${fallback_output_name}${output_suffix}")
    endif()

    add_command(add_test
      "${fallback_name}"
      ${_TEST_EXECUTOR}
      "${_TEST_EXECUTABLE}"
      ${spec}
      ${extra_args}
      "${reporter_arg}"
      "${output_dir_arg}"
    )
    if(labels)
      add_command(set_tests_properties
        "${fallback_name}"
        PROPERTIES
        WORKING_DIRECTORY "${_TEST_WORKING_DIR}"
        ${properties}
        LABELS "${labels}"
      )
    else()
      add_command(set_tests_properties
        "${fallback_name}"
        PROPERTIES
        WORKING_DIRECTORY "${_TEST_WORKING_DIR}"
        ${properties}
      )
    endif()

    if(environment_modifications)
      add_command(set_tests_properties
        "${fallback_name}"
        PROPERTIES
        ENVIRONMENT_MODIFICATION "${environment_modifications}")
    endif()

    list(APPEND tests "${fallback_name}")
    add_command(set ${_TEST_LIST} ${tests})
    file(WRITE "${_CTEST_FILE}" "${script}")
    return()
  endif()

  foreach(line ${output_list})
    if(line STREQUAL "")
      continue()
    endif()
    set(test "${line}")
    set(test_name "${test}")
    foreach(char \\ , [ ])
      string(REPLACE ${char} "\\${char}" test_name "${test_name}")
    endforeach()

    if(output_dir)
      string(REGEX REPLACE "[^A-Za-z0-9_]" "_" test_name_clean "${test_name}")
      set(output_dir_arg "--out ${output_dir}/${output_prefix}${test_name_clean}${output_suffix}")
    else()
      set(output_dir_arg "")
    endif()

    add_command(add_test
      "${prefix}${test}${suffix}"
      ${_TEST_EXECUTOR}
      "${_TEST_EXECUTABLE}"
      "${test_name}"
      ${extra_args}
      "${reporter_arg}"
      "${output_dir_arg}"
    )
    if(labels)
      add_command(set_tests_properties
        "${prefix}${test}${suffix}"
        PROPERTIES
        WORKING_DIRECTORY "${_TEST_WORKING_DIR}"
        ${properties}
        LABELS "${labels}"
      )
    else()
      add_command(set_tests_properties
        "${prefix}${test}${suffix}"
        PROPERTIES
        WORKING_DIRECTORY "${_TEST_WORKING_DIR}"
        ${properties}
      )
    endif()

    if(environment_modifications)
      add_command(set_tests_properties
        "${prefix}${test}${suffix}"
        PROPERTIES
        ENVIRONMENT_MODIFICATION "${environment_modifications}")
    endif()

    list(APPEND tests "${prefix}${test}${suffix}")
  endforeach()

  add_command(set ${_TEST_LIST} ${tests})
  file(WRITE "${_CTEST_FILE}" "${script}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
  set(_pulp_label_args)
  if(TEST_LABELS)
    list(APPEND _pulp_label_args TEST_LABELS ${TEST_LABELS})
  endif()
  catch_discover_tests_impl(
    TEST_TARGET ${TEST_TARGET}
    TEST_EXECUTABLE ${TEST_EXECUTABLE}
    TEST_EXECUTOR ${TEST_EXECUTOR}
    TEST_WORKING_DIR ${TEST_WORKING_DIR}
    TEST_SPEC ${TEST_SPEC}
    TEST_EXTRA_ARGS ${TEST_EXTRA_ARGS}
    TEST_PROPERTIES ${TEST_PROPERTIES}
    ${_pulp_label_args}
    TEST_PREFIX ${TEST_PREFIX}
    TEST_SUFFIX ${TEST_SUFFIX}
    TEST_LIST ${TEST_LIST}
    TEST_REPORTER ${TEST_REPORTER}
    TEST_OUTPUT_DIR ${TEST_OUTPUT_DIR}
    TEST_OUTPUT_PREFIX ${TEST_OUTPUT_PREFIX}
    TEST_OUTPUT_SUFFIX ${TEST_OUTPUT_SUFFIX}
    TEST_DL_PATHS ${TEST_DL_PATHS}
    TEST_DL_FRAMEWORK_PATHS ${TEST_DL_FRAMEWORK_PATHS}
    CTEST_FILE ${CTEST_FILE}
  )
endif()
