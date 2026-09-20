cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and PULP_BUILD_DIR are required")
endif()

set(_root "${PULP_BUILD_DIR}/macho-linked-runtime-staging-smoke")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}/src")
file(MAKE_DIRECTORY "${_root}/src/attribution")
foreach(_attribution_file LICENSE.md NOTICE.md DEPENDENCIES.md)
    file(WRITE "${_root}/src/attribution/${_attribution_file}"
        "${_attribution_file} fixture\n")
endforeach()
file(WRITE "${_root}/src/runtime.cpp"
    "extern \"C\" int proof_runtime() { return 7; }\n")
file(WRITE "${_root}/src/linked.cpp"
    "extern \"C\" int proof_runtime();\n"
    "int main() { return proof_runtime() == 7 ? 0 : 1; }\n")
file(WRITE "${_root}/src/unlinked.cpp" "int main() { return 0; }\n")
file(WRITE "${_root}/src/decoy.cpp"
    "extern \"C\" int proof_decoy() { return 11; }\n")
file(WRITE "${_root}/src/spaced.cpp"
    "extern \"C\" int proof_spaced_runtime() { return 13; }\n")
file(WRITE "${_root}/src/substring.cpp"
    "extern \"C\" int proof_decoy();\n"
    "int main() { return proof_decoy() == 11 ? 0 : 1; }\n")
file(WRITE "${_root}/src/spaced_linked.cpp"
    "extern \"C\" int proof_spaced_runtime();\n"
    "int main() { return proof_spaced_runtime() == 13 ? 0 : 1; }\n")
file(WRITE "${_root}/src/transition.cpp"
    "extern \"C\" int proof_runtime();\n"
    "int main() {\n"
    "#if PROOF_TRANSITION_LINK_RUNTIME\n"
    "    return proof_runtime() == 7 ? 0 : 1;\n"
    "#else\n"
    "    return 0;\n"
    "#endif\n"
    "}\n")
file(WRITE "${_root}/src/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(macho_linked_runtime_staging LANGUAGES CXX)
include("${PULP_RUNTIME_STAGING_MODULE}")
option(PROOF_TRANSITION_LINK_RUNTIME "Link transition proof to runtime" ON)

add_library(ProofRuntime SHARED runtime.cpp)
set_target_properties(ProofRuntime PROPERTIES OUTPUT_NAME proof-runtime)
set_property(TARGET ProofRuntime PROPERTY PULP_RUNTIME_ATTRIBUTION_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/attribution")
set_property(TARGET ProofRuntime PROPERTY PULP_RUNTIME_ATTRIBUTION_NAME
    "Vellum")
pulp_register_macho_linked_runtime_dependency_target(ProofRuntime)

# Absolute Mach-O install names may contain spaces. The linked-image parser
# must retain and compare the complete install identity.
add_library(ProofSpacedRuntime SHARED spaced.cpp)
set_target_properties(ProofSpacedRuntime PROPERTIES
    OUTPUT_NAME proof-spaced-runtime
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Provider Support"
    INSTALL_NAME_DIR "${CMAKE_BINARY_DIR}/Provider Support"
    BUILD_WITH_INSTALL_NAME_DIR TRUE)
pulp_register_macho_linked_runtime_dependency_target(ProofSpacedRuntime)

# This runtime deliberately contains the registered runtime's full filename as
# the exact basename while using a different Mach-O install identity. Staging
# must compare the registered dylib identity rather than treating this collision
# as evidence that ProofRuntime is linked.
add_library(ProofRuntimeDecoy SHARED decoy.cpp)
set_target_properties(ProofRuntimeDecoy PROPERTIES
    OUTPUT_NAME proof-runtime
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/decoy"
    INSTALL_NAME_DIR "@rpath/decoy"
    BUILD_WITH_INSTALL_NAME_DIR TRUE)

add_executable(ProofLinked linked.cpp)
set_target_properties(ProofLinked PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/linked/$<CONFIG>")
target_link_libraries(ProofLinked PRIVATE ProofRuntime)
pulp_stage_runtime_dependencies(ProofLinked)

add_executable(ProofUnlinked unlinked.cpp)
set_target_properties(ProofUnlinked PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unlinked/$<CONFIG>")
pulp_stage_runtime_dependencies(ProofUnlinked)

add_executable(ProofSubstring substring.cpp)
set_target_properties(ProofSubstring PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/substring/$<CONFIG>")
target_link_libraries(ProofSubstring PRIVATE ProofRuntimeDecoy)
pulp_stage_runtime_dependencies(ProofSubstring)

add_executable(ProofSpacedLinked spaced_linked.cpp)
set_target_properties(ProofSpacedLinked PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/spaced-linked/$<CONFIG>")
target_link_libraries(ProofSpacedLinked PRIVATE ProofSpacedRuntime)
pulp_stage_runtime_dependencies(ProofSpacedLinked)

add_executable(ProofTransition transition.cpp)
set_target_properties(ProofTransition PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/transition/$<CONFIG>")
if(PROOF_TRANSITION_LINK_RUNTIME)
    target_compile_definitions(ProofTransition PRIVATE
        PROOF_TRANSITION_LINK_RUNTIME=1)
    target_link_libraries(ProofTransition PRIVATE ProofRuntime)
else()
    target_compile_definitions(ProofTransition PRIVATE
        PROOF_TRANSITION_LINK_RUNTIME=0)
endif()
pulp_stage_runtime_dependencies(ProofTransition)

foreach(_shared_pair LinkedFirst UnlinkedFirst)
    add_executable(ProofShared${_shared_pair}Linked EXCLUDE_FROM_ALL linked.cpp)
    set_target_properties(ProofShared${_shared_pair}Linked PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/shared-${_shared_pair}/$<CONFIG>")
    target_link_libraries(ProofShared${_shared_pair}Linked PRIVATE ProofRuntime)
    pulp_stage_runtime_dependencies(ProofShared${_shared_pair}Linked)

    add_executable(ProofShared${_shared_pair}Unlinked EXCLUDE_FROM_ALL unlinked.cpp)
    set_target_properties(ProofShared${_shared_pair}Unlinked PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/shared-${_shared_pair}/$<CONFIG>")
    pulp_stage_runtime_dependencies(ProofShared${_shared_pair}Unlinked)
endforeach()
]=])

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${_root}/src"
        -B "${_root}/build"
        -DCMAKE_BUILD_TYPE=Release
        "-DPULP_RUNTIME_STAGING_MODULE=${PULP_SOURCE_DIR}/tools/cmake/PulpRuntimeStaging.cmake"
    RESULT_VARIABLE _configure_rc
    OUTPUT_VARIABLE _configure_out
    ERROR_VARIABLE _configure_err)
if(NOT _configure_rc EQUAL 0)
    message(FATAL_ERROR
        "Mach-O linked-runtime fixture configure failed (${_configure_rc})\n"
        "${_configure_out}\n${_configure_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_root}/build" --config Release
    RESULT_VARIABLE _build_rc
    OUTPUT_VARIABLE _build_out
    ERROR_VARIABLE _build_err)
if(NOT _build_rc EQUAL 0)
    message(FATAL_ERROR
        "Mach-O linked-runtime fixture build failed (${_build_rc})\n"
        "${_build_out}\n${_build_err}")
endif()

set(_runtime_name "libproof-runtime.dylib")
set(_spaced_runtime_name "libproof-spaced-runtime.dylib")
set(_linked_dir "${_root}/build/linked/Release")
set(_unlinked_dir "${_root}/build/unlinked/Release")
set(_substring_dir "${_root}/build/substring/Release")
set(_spaced_linked_dir "${_root}/build/spaced-linked/Release")
set(_transition_dir "${_root}/build/transition/Release")
if(NOT EXISTS "${_linked_dir}/${_runtime_name}")
    message(FATAL_ERROR "linked target did not receive ${_runtime_name}")
endif()
foreach(_attribution_file LICENSE.md NOTICE.md DEPENDENCIES.md)
    if(NOT EXISTS
       "${_linked_dir}/PulpThirdParty/Vellum/${_attribution_file}")
        message(FATAL_ERROR
            "linked target did not receive ${_attribution_file} attribution")
    endif()
    if(EXISTS
       "${_unlinked_dir}/PulpThirdParty/Vellum/${_attribution_file}"
       OR EXISTS
       "${_substring_dir}/PulpThirdParty/Vellum/${_attribution_file}")
        message(FATAL_ERROR
            "unlinked target incorrectly received ${_attribution_file}")
    endif()
endforeach()
if(EXISTS "${_unlinked_dir}/${_runtime_name}")
    message(FATAL_ERROR "unlinked target incorrectly received ${_runtime_name}")
endif()
if(EXISTS "${_substring_dir}/${_runtime_name}")
    message(FATAL_ERROR
        "substring-only load command incorrectly received ${_runtime_name}")
endif()
if(NOT EXISTS "${_spaced_linked_dir}/${_spaced_runtime_name}")
    message(FATAL_ERROR
        "spaced-path target did not receive ${_spaced_runtime_name}")
endif()
if(NOT EXISTS "${_transition_dir}/${_runtime_name}")
    message(FATAL_ERROR
        "initial linked transition target did not receive ${_runtime_name}")
endif()
if(NOT _build_out MATCHES "ProofLinked: verified ${_runtime_name}")
    message(FATAL_ERROR "linked verification receipt missing:\n${_build_out}")
endif()
if(NOT _build_out MATCHES "ProofUnlinked: skipped unlinked ${_runtime_name}")
    message(FATAL_ERROR "unlinked skip receipt missing:\n${_build_out}")
endif()
if(NOT _build_out MATCHES
   "ProofSubstring: skipped unlinked ${_runtime_name}")
    message(FATAL_ERROR "substring skip receipt missing:\n${_build_out}")
endif()
if(NOT _build_out MATCHES
   "ProofSpacedLinked: verified ${_spaced_runtime_name}")
    message(FATAL_ERROR "spaced-path verification receipt missing:\n${_build_out}")
endif()

# Two consumers may intentionally share one output directory. Prove that an
# unlinked consumer cannot remove a provider still loaded by its peer, and that
# build order does not change the final payload.
foreach(_shared_pair LinkedFirst UnlinkedFirst)
    if(_shared_pair STREQUAL "LinkedFirst")
        set(_shared_build_order
            ProofSharedLinkedFirstLinked ProofSharedLinkedFirstUnlinked)
    else()
        set(_shared_build_order
            ProofSharedUnlinkedFirstUnlinked ProofSharedUnlinkedFirstLinked)
    endif()
    foreach(_shared_target IN LISTS _shared_build_order)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" --build "${_root}/build"
                --config Release --target "${_shared_target}"
            RESULT_VARIABLE _shared_build_rc
            OUTPUT_VARIABLE _shared_build_out
            ERROR_VARIABLE _shared_build_err)
        if(NOT _shared_build_rc EQUAL 0)
            message(FATAL_ERROR
                "shared-output ${_shared_target} build failed "
                "(${_shared_build_rc})\n${_shared_build_out}\n${_shared_build_err}")
        endif()
    endforeach()
    set(_shared_dir "${_root}/build/shared-${_shared_pair}/Release")
    if(NOT EXISTS "${_shared_dir}/${_runtime_name}")
        message(FATAL_ERROR
            "shared-output ${_shared_pair} lost ${_runtime_name}")
    endif()
    foreach(_attribution_file LICENSE.md NOTICE.md DEPENDENCIES.md)
        if(NOT EXISTS
           "${_shared_dir}/PulpThirdParty/Vellum/${_attribution_file}")
            message(FATAL_ERROR
                "shared-output ${_shared_pair} lost ${_attribution_file}")
        endif()
    endforeach()
endforeach()

# Reconfigure the same target without its provider dependency and rebuild in
# place. The old provider and attribution must be removed from the existing
# output directory rather than surviving as stale packaging payload.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${_root}/src"
        -B "${_root}/build"
        -DCMAKE_BUILD_TYPE=Release
        -DPROOF_TRANSITION_LINK_RUNTIME=OFF
        "-DPULP_RUNTIME_STAGING_MODULE=${PULP_SOURCE_DIR}/tools/cmake/PulpRuntimeStaging.cmake"
    RESULT_VARIABLE _transition_configure_rc
    OUTPUT_VARIABLE _transition_configure_out
    ERROR_VARIABLE _transition_configure_err)
if(NOT _transition_configure_rc EQUAL 0)
    message(FATAL_ERROR
        "transition fixture reconfigure failed (${_transition_configure_rc})\n"
        "${_transition_configure_out}\n${_transition_configure_err}")
endif()
# The first configure/build and the transition reconfigure can complete within
# one filesystem timestamp tick. Make the source observably newer so the test
# exercises an actual incremental relink rather than an up-to-date no-op.
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(APPEND "${_root}/src/transition.cpp"
    "\n// Force the linked-to-unlinked transition relink.\n")
# Simulate an SDK/provider update between the linked build and cleanup. The
# ownership receipt must compare staged bytes with the digests it recorded at
# staging time, rather than with these newly changed provider/source files.
file(APPEND "${_root}/build/${_runtime_name}" "provider-update")
foreach(_attribution_file LICENSE.md NOTICE.md DEPENDENCIES.md)
    file(APPEND "${_root}/src/attribution/${_attribution_file}"
        "provider update\n")
endforeach()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_root}/build" --config Release
        --target ProofTransition
    RESULT_VARIABLE _transition_build_rc
    OUTPUT_VARIABLE _transition_build_out
    ERROR_VARIABLE _transition_build_err)
if(NOT _transition_build_rc EQUAL 0)
    message(FATAL_ERROR
        "transition fixture rebuild failed (${_transition_build_rc})\n"
        "${_transition_build_out}\n${_transition_build_err}")
endif()
if(EXISTS "${_transition_dir}/${_runtime_name}")
    message(FATAL_ERROR
        "linked-to-unlinked transition retained stale ${_runtime_name}")
endif()
foreach(_attribution_file LICENSE.md NOTICE.md DEPENDENCIES.md)
    if(EXISTS
       "${_transition_dir}/PulpThirdParty/Vellum/${_attribution_file}")
        message(FATAL_ERROR
            "linked-to-unlinked transition retained stale ${_attribution_file}")
    endif()
endforeach()
if(NOT _transition_build_out MATCHES
   "ProofTransition: skipped unlinked ${_runtime_name}")
    message(FATAL_ERROR
        "transition skip receipt missing:\n${_transition_build_out}")
endif()

execute_process(
    COMMAND /usr/bin/otool -L "${_linked_dir}/ProofLinked"
    RESULT_VARIABLE _otool_rc
    OUTPUT_VARIABLE _otool_out
    ERROR_VARIABLE _otool_err)
if(NOT _otool_rc EQUAL 0
   OR NOT _otool_out MATCHES "@rpath/${_runtime_name}")
    message(FATAL_ERROR
        "linked proof lacks @rpath/${_runtime_name}:\n${_otool_out}\n${_otool_err}")
endif()

message(STATUS "macho_linked_runtime_staging_verified=true")
