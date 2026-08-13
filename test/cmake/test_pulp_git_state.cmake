cmake_minimum_required(VERSION 3.24)

if(NOT PULP_SRC_DIR OR NOT FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SRC_DIR and FIXTURE_DIR are required")
endif()

unset(ENV{GIT_DIR})
unset(ENV{GIT_WORK_TREE})
find_package(Git REQUIRED)
include("${PULP_SRC_DIR}/tools/cmake/PulpGitState.cmake")

set(_repo "${FIXTURE_DIR}/source")
file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${_repo}")

function(run_git)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_repo}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "git ${ARGN} failed (${_result}):\n${_stdout}\n${_stderr}")
    endif()
endfunction()

run_git(init --quiet)
file(WRITE "${_repo}/tracked.txt" "clean\n")
run_git(add tracked.txt)
run_git(-c user.name=Test -c user.email=test@example.com
    commit --quiet -m "fixture")

pulp_git_dirty("${_repo}" _clean_dirty)
if(_clean_dirty)
    message(FATAL_ERROR "clean source was reported dirty")
endif()

file(WRITE "${_repo}/configure-input.txt" "untracked\n")
run_git(config status.showUntrackedFiles no)
pulp_git_dirty("${_repo}" _untracked_dirty)
if(NOT _untracked_dirty)
    message(FATAL_ERROR "untracked configure input was not reported dirty")
endif()
file(REMOVE "${_repo}/configure-input.txt")

set(_submodule_origin "${FIXTURE_DIR}/submodule-origin")
file(MAKE_DIRECTORY "${_submodule_origin}")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_submodule_origin}" init --quiet
    RESULT_VARIABLE _submodule_init_result)
if(NOT _submodule_init_result EQUAL 0)
    message(FATAL_ERROR "could not initialize submodule fixture")
endif()
file(WRITE "${_submodule_origin}/tracked.txt" "clean\n")
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_submodule_origin}" add tracked.txt
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_submodule_origin}"
        -c user.name=Test -c user.email=test@example.com
        commit --quiet -m "submodule fixture"
    COMMAND_ERROR_IS_FATAL ANY)
run_git(-c protocol.file.allow=always submodule add --quiet
    "${_submodule_origin}" module)
run_git(add .gitmodules module)
run_git(-c user.name=Test -c user.email=test@example.com
    commit --quiet -m "add submodule fixture")

file(WRITE "${_repo}/module/configure-input.txt" "untracked\n")
run_git(config diff.ignoreSubmodules all)
run_git(config submodule.module.ignore all)
pulp_git_dirty("${_repo}" _submodule_untracked_dirty)
if(NOT _submodule_untracked_dirty)
    message(FATAL_ERROR
        "untracked submodule input was not reported dirty")
endif()
file(REMOVE "${_repo}/module/configure-input.txt")

file(WRITE "${_repo}/module/tracked.txt" "dirty\n")
pulp_git_dirty("${_repo}" _submodule_tracked_dirty)
if(NOT _submodule_tracked_dirty)
    message(FATAL_ERROR "modified tracked submodule source was not reported dirty")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_repo}/module" checkout -- tracked.txt
    COMMAND_ERROR_IS_FATAL ANY)

file(WRITE "${_repo}/tracked.txt" "dirty\n")
pulp_git_dirty("${_repo}" _tracked_dirty)
if(NOT _tracked_dirty)
    message(FATAL_ERROR "modified tracked source was not reported dirty")
endif()
