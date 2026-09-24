# PulpTestSuite.cmake — shared helper for declaring Catch2 unit-test binaries.
#
# Keeps test/CMakeLists.txt from owning thousands of repeated target lines. The
# common 3-line pattern
#
#     add_executable(pulp-test-X test_X.cpp)
#     target_link_libraries(pulp-test-X PRIVATE pulp::Y Catch2::Catch2WithMain)
#     catch_discover_tests(pulp-test-X)
#
# collapses to a single
#
#     pulp_add_test_suite(pulp-test-X LIBRARIES pulp::Y)
#
# saving 2 lines per migrated test plus consolidating the
# catch_discover_tests() wiring in one place so future Catch2-config
# changes only need to touch this helper.
#
# Validation goal: normalized `ctest -N -V` output stays identical for every
# migrated test. The helper always uses the
# `pulp-test-<name>` convention and always links Catch2WithMain, so
# discovered test names and properties are preserved byte-for-byte
# unless an EXTRA_PROPERTIES override is passed in.

include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/PulpTestTimeout.cmake)

# ---------------------------------------------------------------------------
# Shared precompiled header for Catch2 suites
#
# Every Catch2 test TU re-parses the Catch2 macros plus the same dozen
# standard-library headers; across the ~1,200 suites that parse dominates the
# test tree's compile cost. Two carrier targets, `pulp-test-pch-cxx20` and
# `pulp-test-pch-cxx23`, each precompile that common prefix once, and every
# eligible Catch2 executable reuses the carrier that matches its effective
# C++ standard via PRECOMPILE_HEADERS_REUSE_FROM. Two carriers exist because
# pulp-format-core publishes cxx_std_23 and clang refuses to load a PCH built
# for a different -std.
#
# Eligibility is decided once per directory, after every manifest in it has
# run, so post-declaration edits (target_sources adding a .mm, a
# target_compile_options(-fno-exceptions)) are seen. A target is skipped, and
# the reason recorded, when reusing the PCH could change what it compiles:
#   * a non-C++ source (ObjC++ .mm / C) — CMake refuses a CXX-only carrier
#     for an OBJCXX TU, and a C TU never shares the header set;
#   * a per-target -f / -std / -O / -x / -m / -U option — clang rejects a PCH
#     across exceptions / RTTI / standard mismatches, so these fail loudly,
#     but they are excluded up front rather than relying on the error;
#   * a per-target definition that reconfigures a header the PCH already
#     parsed (_LIBCPP_* / _GLIBCXX_* / CATCH_* / NOMINMAX / ...) — clang
#     accepts a consumer macro the PCH never saw *silently*, which is the one
#     mismatch that changes semantics without an error;
#   * an explicit CXX_STANDARD / CXX_EXTENSIONS other than the carrier's;
#   * NO_PCH on the pulp_add_test_suite call (or PULP_TEST_NO_PCH on a raw
#     add_executable target).
# Extra consumer-only definitions (the -D<fixture path> most suites carry) are
# fine: clang honors them for the TU and the PCH'd headers do not read them.
#
# The decisions are written to <build>/pulp-test-pch.tsv; the `test-pch-wiring`
# ctest cross-checks that ledger against the generated compile lines.
#
# PULP_TEST_PCH=OFF disables the whole mechanism (every suite compiles exactly
# as before). It defaults ON only for Clang, where it is measured; GCC PCH is
# silently ignored on any flag mismatch and MSVC needs the carrier object
# linked, neither of which this wiring has been validated against.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(_pulp_test_pch_default ON)
else()
    set(_pulp_test_pch_default OFF)
endif()
option(PULP_TEST_PCH
    "Share one precompiled Catch2 + standard-library header across Catch2 test executables (Clang only)"
    ${_pulp_test_pch_default})

# Headers precompiled into each carrier. Only Catch2 and the standard library:
# a Pulp header here would tie every suite's rebuild to it.
set(PULP_TEST_PCH_HEADERS
    <catch2/catch_test_macros.hpp>
    <catch2/catch_approx.hpp>
    <catch2/matchers/catch_matchers_floating_point.hpp>
    <catch2/matchers/catch_matchers_string.hpp>
    <algorithm> <array> <atomic> <chrono> <cmath> <cstddef> <cstdint> <cstdio>
    <cstdlib> <cstring> <filesystem> <fstream> <functional> <limits> <memory>
    <numbers> <optional> <span> <sstream> <string> <string_view> <thread>
    <type_traits> <utility> <vector>
    CACHE STRING "Headers precompiled into the shared Catch2 test PCH")
mark_as_advanced(PULP_TEST_PCH_HEADERS)

# Per-target definitions that reconfigure a header the PCH has already parsed.
set(_PULP_TEST_PCH_SENSITIVE_DEFINE_REGEX
    "^-?D?(_LIBCPP_|_GLIBCXX_|CATCH_|_CRT_|NOMINMAX|_USE_MATH_DEFINES|__STDC_)")

# Directory that owns the carrier targets. Pinning it keeps the carriers'
# inherited directory flags stable whatever order the tree's directories are
# processed in; a suite declared elsewhere (an example's test) reuses them only
# if it is processed after this directory and its directory flags match.
set(PULP_TEST_PCH_CARRIER_DIR "${CMAKE_SOURCE_DIR}/test" CACHE PATH
    "Source directory whose scope creates the shared Catch2 test PCH carriers")
mark_as_advanced(PULP_TEST_PCH_CARRIER_DIR)

# Returns (in ${out}) the carrier target for C++ standard ${std}, creating it
# on first use inside PULP_TEST_PCH_CARRIER_DIR, or "" when called from another
# directory before the carrier exists. The carrier is a static library with
# one stub TU; consumers never link it, they only reuse its PCH.
function(_pulp_test_pch_carrier out std)
    set(_carrier "pulp-test-pch-cxx${std}")
    if(NOT TARGET ${_carrier})
        if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL PULP_TEST_PCH_CARRIER_DIR)
            set(${out} "" PARENT_SCOPE)
            return()
        endif()
        set(_stub "${CMAKE_BINARY_DIR}/pulp-test-pch/cxx${std}_stub.cpp")
        if(NOT EXISTS "${_stub}")
            file(WRITE "${_stub}"
                "// Stub translation unit: it exists so CMake has a compile\n"
                "// step to attach the shared Catch2 test PCH to.\n"
                "namespace pulp::test_pch { int carrier_cxx${std}() { return ${std}; } }\n")
        endif()
        add_library(${_carrier} STATIC EXCLUDE_FROM_ALL "${_stub}")
        # Targets inherit the directory's COMPILE_OPTIONS / COMPILE_DEFINITIONS
        # at creation. Remember what this carrier inherited so a consumer
        # created under different directory flags is refused, not mismatched.
        get_property(_dir_opts DIRECTORY PROPERTY COMPILE_OPTIONS)
        get_property(_dir_defs DIRECTORY PROPERTY COMPILE_DEFINITIONS)
        set_target_properties(${_carrier} PROPERTIES
            CXX_STANDARD ${std}
            CXX_STANDARD_REQUIRED ON
            FOLDER "test-pch"
            PULP_TEST_PCH_DIRECTORY_OPTIONS "${_dir_opts}"
            PULP_TEST_PCH_DIRECTORY_DEFINITIONS "${_dir_defs}")
        # Include dirs only: linking Catch2 would pull its usage requirements
        # (definitions, features) into the carrier, and any definition the
        # PCH is built with but a consumer lacks is accepted by clang without
        # a diagnostic. The carrier must carry no definition of its own.
        target_include_directories(${_carrier} PRIVATE
            $<TARGET_PROPERTY:Catch2::Catch2,INTERFACE_INCLUDE_DIRECTORIES>)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # Without this clang stamps the build time into the .pch, so a
            # regenerated PCH differs byte-for-byte and every consumer misses
            # in ccache. With it, the same inputs produce the same .pch.
            target_compile_options(${_carrier} PRIVATE -Xclang -fno-pch-timestamp)
        endif()
        set(_headers "")
        foreach(_h IN LISTS PULP_TEST_PCH_HEADERS)
            list(APPEND _headers "$<$<COMPILE_LANGUAGE:CXX>:${_h}>")
        endforeach()
        target_precompile_headers(${_carrier} PRIVATE ${_headers})
    endif()
    set(${out} "${_carrier}" PARENT_SCOPE)
endfunction()

# Effective C++ standard of an executable, resolved the way CMake will at
# generate time: its CXX_STANDARD property (every target gets one from
# CMAKE_CXX_STANDARD at creation; pulp-format sets 23 explicitly) raised by the
# largest cxx_std_N compile feature reachable through the target's own
# COMPILE_FEATURES and the INTERFACE_COMPILE_FEATURES of its transitive,
# non-LINK_ONLY link interface. The same walk reports, in ${out}_sensitive_define,
# the first INTERFACE_COMPILE_DEFINITIONS entry that would reconfigure a
# header the PCH already parsed (empty when there is none).
function(_pulp_test_pch_effective_std out target)
    set(_sensitive "")
    get_target_property(_std ${target} CXX_STANDARD)
    if(NOT _std)
        set(_std "${CMAKE_CXX_STANDARD}")
    endif()
    if(NOT _std)
        set(_std 0)
    endif()
    set(_queue "${target}")
    set(_seen "")
    list(LENGTH _queue _pending)
    while(_pending GREATER 0)
        list(POP_FRONT _queue _t)
        list(LENGTH _queue _pending)
        if(NOT TARGET "${_t}")
            continue()
        endif()
        get_target_property(_aliased "${_t}" ALIASED_TARGET)
        if(_aliased)
            set(_t "${_aliased}")
        endif()
        if("${_t}" IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_t}")
        get_target_property(_type "${_t}" TYPE)
        set(_features "")
        set(_links "")
        if("${_t}" STREQUAL "${target}")
            get_target_property(_features "${_t}" COMPILE_FEATURES)
            get_target_property(_links "${_t}" LINK_LIBRARIES)
        endif()
        get_target_property(_ifeatures "${_t}" INTERFACE_COMPILE_FEATURES)
        get_target_property(_ilinks "${_t}" INTERFACE_LINK_LIBRARIES)
        get_target_property(_idefs "${_t}" INTERFACE_COMPILE_DEFINITIONS)
        foreach(_v IN ITEMS _features _links _ifeatures _ilinks _idefs)
            if(NOT ${_v})
                set(${_v} "")  # unset properties read back as <var>-NOTFOUND
            endif()
        endforeach()
        foreach(_d IN LISTS _idefs)
            if(NOT _sensitive AND _d MATCHES "${_PULP_TEST_PCH_SENSITIVE_DEFINE_REGEX}")
                set(_sensitive "${_t}:${_d}")
            endif()
        endforeach()
        foreach(_f IN LISTS _features _ifeatures)
            if(_f MATCHES "^cxx_std_([0-9]+)$" AND CMAKE_MATCH_1 GREATER _std)
                set(_std "${CMAKE_MATCH_1}")
            endif()
        endforeach()
        foreach(_l IN LISTS _links _ilinks)
            if(_l MATCHES "^\\$<LINK_ONLY:")
                continue()  # private deps of a dependency carry no compile features
            endif()
            if(_l MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
                set(_l "${CMAKE_MATCH_1}")
            endif()
            if(_l MATCHES "\\$<")
                continue()
            endif()
            list(APPEND _queue "${_l}")
        endforeach()
        list(LENGTH _queue _pending)
    endwhile()
    set(${out} "${_std}" PARENT_SCOPE)
    set(${out}_sensitive_define "${_sensitive}" PARENT_SCOPE)
endfunction()

# Classifies one executable: sets ${out} to "pch <carrier>" or "skip <reason>".
function(_pulp_test_pch_classify out target)
    get_target_property(_no_pch ${target} PULP_TEST_NO_PCH)
    if(_no_pch)
        set(${out} "skip\tno-pch-requested" PARENT_SCOPE)
        return()
    endif()
    get_target_property(_sources ${target} SOURCES)
    foreach(_s IN LISTS _sources)
        if(_s MATCHES "^\\$<TARGET_OBJECTS:[^>]+>$")
            continue()  # already-compiled objects, not a TU of this target
        endif()
        if(_s MATCHES "^\\$<\\$<.*>:([^<>]+)>$")
            # A conditionally listed source ($<$<BOOL:...>:path>): either
            # absent or compiled as the path says, so judge the path.
            set(_s "${CMAKE_MATCH_1}")
        endif()
        if(_s MATCHES "\\$<")
            set(${out} "skip\tgenex-source:${_s}" PARENT_SCOPE)
            return()
        endif()
        get_source_file_property(_lang "${_s}" TARGET_DIRECTORY ${target} LANGUAGE)
        if(_lang AND NOT _lang STREQUAL "CXX")
            set(${out} "skip\tsource-language:${_lang}" PARENT_SCOPE)
            return()
        endif()
        if(_s MATCHES "\\.(mm|m|M)$")
            set(${out} "skip\tobjcxx-source" PARENT_SCOPE)
            return()
        endif()
        if(NOT _s MATCHES "\\.(cpp|cc|cxx|hpp|h|hh|hxx|inl|ipp)$")
            set(${out} "skip\tnon-cxx-source:${_s}" PARENT_SCOPE)
            return()
        endif()
        get_source_file_property(_sopts "${_s}" TARGET_DIRECTORY ${target} COMPILE_OPTIONS)
        get_source_file_property(_sflags "${_s}" TARGET_DIRECTORY ${target} COMPILE_FLAGS)
        if(_sopts OR _sflags)
            set(${out} "skip\tper-source-flags:${_s}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    get_target_property(_flags ${target} COMPILE_FLAGS)
    if(_flags)
        set(${out} "skip\tcompile-flags" PARENT_SCOPE)
        return()
    endif()
    # Only the target's OWN options and definitions matter: its directory's
    # are inherited by carrier and consumer alike (compared below).
    get_target_property(_src_dir ${target} SOURCE_DIR)
    get_property(_dir_opts DIRECTORY "${_src_dir}" PROPERTY COMPILE_OPTIONS)
    get_property(_dir_defs DIRECTORY "${_src_dir}" PROPERTY COMPILE_DEFINITIONS)
    get_target_property(_opts ${target} COMPILE_OPTIONS)
    if(_opts AND _dir_opts)
        list(REMOVE_ITEM _opts ${_dir_opts})
    endif()
    foreach(_o IN LISTS _opts)
        if(_o MATCHES "\\$<")
            set(${out} "skip\tgenex-compile-option:${_o}" PARENT_SCOPE)
            return()
        endif()
        if(_o MATCHES "^-(f|std=|O|X|x|m|U|nostd|stdlib|include|isystem)"
                OR _o MATCHES "^/(EH|GR|std:|O)")
            set(${out} "skip\tcompile-option:${_o}" PARENT_SCOPE)
            return()
        endif()
        if(_o MATCHES "${_PULP_TEST_PCH_SENSITIVE_DEFINE_REGEX}")
            set(${out} "skip\tcompile-option:${_o}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    get_target_property(_defs ${target} COMPILE_DEFINITIONS)
    if(_defs AND _dir_defs)
        list(REMOVE_ITEM _defs ${_dir_defs})
    endif()
    foreach(_d IN LISTS _defs)
        if(_d MATCHES "${_PULP_TEST_PCH_SENSITIVE_DEFINE_REGEX}")
            set(${out} "skip\tcompile-definition:${_d}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    _pulp_test_pch_effective_std(_std ${target})
    if(_std_sensitive_define)
        set(${out} "skip\tinterface-definition:${_std_sensitive_define}" PARENT_SCOPE)
        return()
    endif()
    if(NOT _std MATCHES "^(20|23)$")
        set(${out} "skip\tcxx-standard:${_std}" PARENT_SCOPE)
        return()
    endif()
    _pulp_test_pch_carrier(_carrier ${_std})
    if(NOT _carrier)
        set(${out} "skip\tcarrier-not-in-scope:cxx${_std}" PARENT_SCOPE)
        return()
    endif()
    get_target_property(_ext ${target} CXX_EXTENSIONS)
    get_target_property(_carrier_ext ${_carrier} CXX_EXTENSIONS)
    if(NOT "${_ext}" STREQUAL "${_carrier_ext}")
        set(${out} "skip\tcxx-extensions:${_ext}" PARENT_SCOPE)
        return()
    endif()
    get_target_property(_carrier_dir_opts ${_carrier} PULP_TEST_PCH_DIRECTORY_OPTIONS)
    get_target_property(_carrier_dir_defs ${_carrier} PULP_TEST_PCH_DIRECTORY_DEFINITIONS)
    if(NOT "${_dir_opts}" STREQUAL "${_carrier_dir_opts}"
            OR NOT "${_dir_defs}" STREQUAL "${_carrier_dir_defs}")
        set(${out} "skip\tdirectory-flags-differ-from-carrier" PARENT_SCOPE)
        return()
    endif()
    set(${out} "pch\t${_carrier}" PARENT_SCOPE)
endfunction()

# Classifies every Catch2 executable declared directly in ${dir} (once: a
# target already in the ledger is left alone), applying the PCH to the
# eligible ones.
function(_pulp_test_pch_process_directory dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_done GLOBAL PROPERTY PULP_TEST_PCH_HANDLED)
    foreach(_t IN LISTS _targets)
        if("${_t}" IN_LIST _done)
            continue()
        endif()
        get_target_property(_type ${_t} TYPE)
        if(NOT _type STREQUAL "EXECUTABLE")
            continue()
        endif()
        get_target_property(_links ${_t} LINK_LIBRARIES)
        if(NOT ("Catch2::Catch2WithMain" IN_LIST _links OR "Catch2::Catch2" IN_LIST _links))
            continue()
        endif()
        if(PULP_TEST_PCH)
            _pulp_test_pch_classify(_verdict ${_t})
        else()
            set(_verdict "off\tPULP_TEST_PCH=OFF")
        endif()
        if(_verdict MATCHES "^pch\t(.+)$")
            target_precompile_headers(${_t} REUSE_FROM ${CMAKE_MATCH_1})
        endif()
        set_property(GLOBAL APPEND PROPERTY PULP_TEST_PCH_HANDLED "${_t}")
        set_property(GLOBAL APPEND PROPERTY PULP_TEST_PCH_LEDGER "${_t}\t${_verdict}")
    endforeach()
endfunction()

# Runs once at the end of every directory that declared a test suite. The
# carrier directory also sweeps every subdirectory beneath it, so a raw
# add_executable Catch2 target in test/web-compat (which never calls
# pulp_add_test_suite) is classified too. Rewrites the ledger afterwards.
function(_pulp_test_pch_finalize_directory)
    set(_dirs "${CMAKE_CURRENT_SOURCE_DIR}")
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL PULP_TEST_PCH_CARRIER_DIR)
        set(_pending "${CMAKE_CURRENT_SOURCE_DIR}")
        list(LENGTH _pending _n)
        while(_n GREATER 0)
            list(POP_FRONT _pending _d)
            get_property(_subs DIRECTORY "${_d}" PROPERTY SUBDIRECTORIES)
            foreach(_sub IN LISTS _subs)
                if(_sub MATCHES "^${CMAKE_SOURCE_DIR}/test/")
                    list(APPEND _dirs "${_sub}")
                    list(APPEND _pending "${_sub}")
                endif()
            endforeach()
            list(LENGTH _pending _n)
        endwhile()
    endif()
    foreach(_d IN LISTS _dirs)
        _pulp_test_pch_process_directory("${_d}")
    endforeach()
    get_property(_ledger GLOBAL PROPERTY PULP_TEST_PCH_LEDGER)
    list(JOIN _ledger "\n" _body)
    file(WRITE "${CMAKE_BINARY_DIR}/pulp-test-pch.tsv"
        "# target\tstatus\tdetail  (status: pch = reuses carrier, skip = excluded, off = PULP_TEST_PCH=OFF)\n"
        "# generated by tools/cmake/PulpTestSuite.cmake at configure time\n"
        "${_body}\n")
endfunction()

# Arms the per-directory finalize pass the first time a suite is declared in
# a directory. Deferred so the pass sees the directory's final target state.
# In the carrier directory both carriers are created up front, so a
# subdirectory it adds later (test/web-compat) finds them at its own finalize.
function(_pulp_test_pch_arm_directory)
    get_property(_armed DIRECTORY PROPERTY _PULP_TEST_PCH_FINALIZE_ARMED)
    if(NOT _armed)
        set_property(DIRECTORY PROPERTY _PULP_TEST_PCH_FINALIZE_ARMED TRUE)
        cmake_language(DEFER CALL _pulp_test_pch_finalize_directory)
        if(PULP_TEST_PCH AND CMAKE_CURRENT_SOURCE_DIR STREQUAL PULP_TEST_PCH_CARRIER_DIR)
            _pulp_test_pch_carrier(_unused 20)
            _pulp_test_pch_carrier(_unused 23)
        endif()
    endif()
endfunction()

# pulp_add_test_suite(NAME
#     [SOURCES src1 src2 ...]            # default: derived "<NAME>.cpp" stripped of leading "pulp-test-"
#     [LIBRARIES lib1 lib2 ...]          # additional Pulp / system libraries (Catch2WithMain is always linked)
#     [TEST_SPEC "spec"]                 # catch_discover_tests TEST_SPEC
#     [TEST_PREFIX "prefix"]             # catch_discover_tests TEST_PREFIX
#     [LABELS "label1;label2"]           # catch_discover_tests PROPERTIES LABELS
#     [TIMEOUT seconds]                  # catch_discover_tests PROPERTIES TIMEOUT
#     [PROPERTIES name value ...]        # additional catch_discover_tests PROPERTIES
#     [DISCOVERY_ARGS arg1 arg2 ...]      # raw catch_discover_tests args
#     [INCLUDE_DIRS dir1 dir2 ...]       # extra include directories
#     [COMPILE_DEFINITIONS def1 ...]     # extra compile definitions
#     [NO_PCH]                           # compile without the shared Catch2 PCH
# )
function(pulp_add_test_suite NAME)
    set(options NO_PCH)
    set(oneValueArgs TIMEOUT TEST_SPEC TEST_PREFIX)
    set(multiValueArgs SOURCES LIBRARIES INCLUDE_DIRS COMPILE_DEFINITIONS PROPERTIES DISCOVERY_ARGS LABELS)
    cmake_parse_arguments(P "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "pulp_add_test_suite(${NAME}): unparsed arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()

    # Default source file: strip the conventional "pulp-test-" prefix and
    # append .cpp. Most existing test targets follow this convention so
    # the SOURCES argument is rarely needed.
    if(NOT P_SOURCES)
        string(REGEX REPLACE "^pulp-test-" "" _stem "${NAME}")
        string(REPLACE "-" "_" _stem "${_stem}")
        set(P_SOURCES "test_${_stem}.cpp")
    endif()

    add_executable(${NAME} ${P_SOURCES})
    target_link_libraries(${NAME} PRIVATE ${P_LIBRARIES} Catch2::Catch2WithMain)

    if(P_INCLUDE_DIRS)
        target_include_directories(${NAME} PRIVATE ${P_INCLUDE_DIRS})
    endif()
    if(P_COMPILE_DEFINITIONS)
        target_compile_definitions(${NAME} PRIVATE ${P_COMPILE_DEFINITIONS})
    endif()
    if(P_NO_PCH)
        set_target_properties(${NAME} PROPERTIES PULP_TEST_NO_PCH TRUE)
    endif()
    _pulp_test_pch_arm_directory()

    # Discover Catch2 cases. Pass LABELS / TIMEOUT / additional properties
    # through if set so downstream `ctest -L <label>` and per-suite timeouts
    # keep working.
    set(_discover_args ${P_DISCOVERY_ARGS})
    if(P_TEST_SPEC)
        list(APPEND _discover_args TEST_SPEC "${P_TEST_SPEC}")
    endif()
    if(P_TEST_PREFIX)
        list(APPEND _discover_args TEST_PREFIX "${P_TEST_PREFIX}")
    endif()
    if(P_LABELS OR P_TIMEOUT OR P_PROPERTIES)
        if(P_TIMEOUT OR P_PROPERTIES)
            list(APPEND _discover_args PROPERTIES)
            if(P_TIMEOUT)
                # Budgets are authored for an optimized build; a coverage tree
                # runs the same work at -O0 with instrumentation. Scaling here
                # keeps every suite's declared TIMEOUT honest in both without
                # each manifest having to know the build config.
                pulp_scaled_test_timeout(_pulp_timeout "${P_TIMEOUT}")
                list(APPEND _discover_args TIMEOUT "${_pulp_timeout}")
            endif()
            if(P_PROPERTIES)
                list(APPEND _discover_args ${P_PROPERTIES})
            endif()
        endif()
        if(P_LABELS)
            # LABELS is a first-class multi-value argument in PulpCatch.cmake.
            # Keep it last so arbitrary CTest property names cannot terminate it.
            list(APPEND _discover_args LABELS ${P_LABELS})
        endif()
    endif()
    catch_discover_tests(${NAME} ${_discover_args})
endfunction()
