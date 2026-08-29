# Consumption census — target-graph facts dump plus the drift gate over
# docs/status/consumption-profiles.json.
#
# The facts dump is the census instrument. CMake is the only place the target
# property graph exists, so configure writes the raw graph (link interfaces,
# source directories, exported include directories) to the build tree and the
# Python generator interprets it. Rewriting it on every configure is what keeps
# the published census from describing a build tree nobody has any more.
#
# Everything here runs inside every configure, so it is written for speed:
# properties are escaped with whole-list string(REPLACE) rather than a
# per-entry loop, and only library targets are dumped. Executables and utility
# targets can never appear in another target's link closure, and skipping them
# drops the dump from ~1350 targets to ~110.

# Collect every buildsystem target under `dir`, recursing into subdirectories.
#
# IMPORTED targets are collected alongside the buildsystem ones. They are not
# optional detail: Skia, Dawn and the platform SDKs reach a consumer only
# through an imported target's own link interface, and treating those as leaves
# silently drops every archive and framework behind them.
function(_pulp_census_collect_targets dir out_var)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_imported DIRECTORY "${dir}" PROPERTY IMPORTED_TARGETS)
    list(APPEND _targets ${_imported})
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _pulp_census_collect_targets("${_sub}" _sub_targets)
        list(APPEND _targets ${_sub_targets})
    endforeach()
    set(${out_var} "${_targets}" PARENT_SCOPE)
endfunction()

# Write the target-graph facts the census generator reads. Deferred to the end
# of the top-level directory so every subsystem, tool and example target the
# build defines is already present, whichever order the subdirectories run in.
function(_pulp_census_write_facts)
    # Feature switches that change which targets exist or what they link, and
    # so change every closure downstream of them. The census refuses to compare
    # two build trees that disagree here.
    set(_feature_vars
        PULP_HAS_SKIA
        PULP_ENABLE_JS
        PULP_BUILD_WEBVIEW
        PULP_HAS_VST3
        PULP_HAS_AUSDK
        PULP_HAS_CLAP
        PULP_HAS_LV2
        PULP_ENABLE_DESIGN_IMPORT
        PULP_ENABLE_INSPECTOR
        PULP_TRACING
        PULP_BUILD_TESTS
        PULP_BUILD_EXAMPLES
    )
    set(_library_types
        STATIC_LIBRARY SHARED_LIBRARY MODULE_LIBRARY
        OBJECT_LIBRARY INTERFACE_LIBRARY UNKNOWN_LIBRARY
    )

    _pulp_census_collect_targets("${CMAKE_SOURCE_DIR}" _all_targets)
    list(REMOVE_DUPLICATES _all_targets)
    list(SORT _all_targets)

    string(LENGTH "${CMAKE_SOURCE_DIR}" _source_dir_length)
    set(_all_entries "")
    set(_out_of_scope "")
    set(_body "")
    set(_sep "")
    foreach(_target IN LISTS _all_targets)
        # An IMPORTED target created inside a subdirectory is scoped to that
        # subdirectory, so it is listed here but cannot be read from the
        # top-level scope this runs in. Record those by name rather than
        # dropping them quietly: they are a hole in the graph, and the census
        # says so instead of reporting a closure that looks complete.
        if(NOT TARGET "${_target}")
            list(APPEND _out_of_scope "${_target}")
            continue()
        endif()
        get_target_property(_type "${_target}" TYPE)
        if(NOT _type IN_LIST _library_types)
            continue()
        endif()
        get_target_property(_is_imported "${_target}" IMPORTED)
        set(_imported_json "false")
        set(_source_dir "")
        set(_export_name "")
        # LINK_LIBRARIES is not readable on an IMPORTED target, and an imported
        # target has no source directory or export name to report.
        set(_target_props INTERFACE_LINK_LIBRARIES INTERFACE_INCLUDE_DIRECTORIES)
        if(_is_imported)
            set(_imported_json "true")
        else()
            # MANUALLY_ADDED_DEPENDENCIES are build-order edges from
            # add_dependencies(). They put nothing on a consumer's link line,
            # but `cmake --graphviz` draws them exactly like link edges, so the
            # census records them to keep the cross-check exact instead of
            # explaining the difference away.
            list(INSERT _target_props 0 LINK_LIBRARIES MANUALLY_ADDED_DEPENDENCIES)
            get_target_property(_source_dir "${_target}" SOURCE_DIR)
            get_target_property(_export_name "${_target}" EXPORT_NAME)
            if(NOT _export_name)
                set(_export_name "")
            endif()
        endif()

        set(_props "")
        set(_prop_sep "")
        foreach(_prop IN LISTS _target_props)
            get_target_property(_value "${_target}" ${_prop})
            set(_json "[]")
            if(_value)
                if(NOT _prop STREQUAL "INTERFACE_INCLUDE_DIRECTORIES")
                    list(APPEND _all_entries ${_value})
                endif()
                # Escape the whole ;-separated list at once, then turn the list
                # separators into JSON array separators.
                string(REPLACE "\\" "\\\\" _value "${_value}")
                string(REPLACE "\"" "\\\"" _value "${_value}")
                string(REPLACE "\n" "\\n" _value "${_value}")
                string(REPLACE ";" "\",\"" _value "${_value}")
                set(_json "[\"${_value}\"]")
            endif()
            string(TOLOWER "${_prop}" _prop_key)
            string(APPEND _props "${_prop_sep}\n      \"${_prop_key}\": ${_json}")
            set(_prop_sep ",")
        endforeach()
        # An object library consumed as $<TARGET_OBJECTS:...> is absorbed into
        # this archive rather than added to a consumer's link line, so it is
        # NOT part of the consumption closure. `cmake --graphviz` draws it like
        # any other dependency, so record the absorbed set: the cross-check
        # needs it to tell a deliberate exclusion from a missed edge.
        set(_absorbed_json "[]")
        if(NOT _is_imported)
            get_target_property(_sources "${_target}" SOURCES)
            if(_sources)
                string(REGEX MATCHALL "\\$<TARGET_OBJECTS:[^>]+>" _object_refs "${_sources}")
                if(_object_refs)
                    list(TRANSFORM _object_refs REPLACE "^\\$<TARGET_OBJECTS:" "")
                    list(TRANSFORM _object_refs REPLACE ">$" "")
                    list(REMOVE_DUPLICATES _object_refs)
                    list(SORT _object_refs)
                    string(REPLACE ";" "\",\"" _absorbed_json "${_object_refs}")
                    set(_absorbed_json "[\"${_absorbed_json}\"]")
                endif()
            endif()
        endif()
        string(APPEND _props ",\n      \"absorbed_object_libraries\": ${_absorbed_json}")
        if(_is_imported)
            string(APPEND _props ",\n      \"link_libraries\": []")
            string(APPEND _props ",\n      \"manually_added_dependencies\": []")
        endif()

        # Targets defined outside the source tree (fetched dependencies) get an
        # empty source_dir: their absolute paths are machine-specific and the
        # census must never carry them. The binary directory is checked FIRST
        # and excluded, because it usually sits inside the source tree — a
        # FetchContent dependency under `build/_deps` is otherwise recorded
        # relative to the source root, which bakes the build directory's name
        # into the published census and makes every other build directory read
        # as drift.
        set(_rel_source_dir "")
        string(FIND "${_source_dir}" "${CMAKE_BINARY_DIR}" _under_binary)
        string(FIND "${_source_dir}" "${CMAKE_SOURCE_DIR}" _under_source)
        if(_under_binary EQUAL 0)
            set(_under_source -1)
        endif()
        if(_under_source EQUAL 0)
            string(SUBSTRING "${_source_dir}" ${_source_dir_length} -1 _rel_source_dir)
            string(REGEX REPLACE "^/" "" _rel_source_dir "${_rel_source_dir}")
        endif()

        string(APPEND _body "${_sep}\n    \"${_target}\": {")
        string(APPEND _body "\n      \"type\": \"${_type}\",")
        string(APPEND _body "\n      \"source_dir\": \"${_rel_source_dir}\",")
        string(APPEND _body "\n      \"export_name\": \"${_export_name}\",")
        string(APPEND _body "\n      \"imported\": ${_imported_json},")
        string(APPEND _body "${_props}")
        string(APPEND _body "\n    }")
        set(_sep ",")
    endforeach()

    # Link entries name targets through aliases (pulp::runtime), imported
    # namespaced targets (SDL3::SDL3-static) and raw linker flags, and CMake is
    # the only thing that can tell those apart. Resolve every identifier the
    # link properties mention so the generator never has to guess which naming
    # convention a dependency arrived under.
    # A namespaced target name (SDL3::SDL3-static) must survive tokenisation
    # while the single colons that separate generator-expression heads from
    # their arguments must not, so hide `::` before splitting and restore it
    # afterwards.
    string(REPLACE "::" "@PULPCENSUSNS@" _entry_text "${_all_entries}")
    string(REGEX MATCHALL "[A-Za-z0-9_.+@-]+" _tokens "${_entry_text}")
    list(TRANSFORM _tokens REPLACE "@PULPCENSUSNS@" "::")
    list(REMOVE_DUPLICATES _tokens)
    list(SORT _tokens)
    set(_resolution "")
    set(_rsep "")
    foreach(_token IN LISTS _tokens)
        if(NOT TARGET "${_token}")
            continue()
        endif()
        get_target_property(_aliased "${_token}" ALIASED_TARGET)
        set(_real "${_token}")
        if(_aliased)
            set(_real "${_aliased}")
        endif()
        get_target_property(_imported "${_real}" IMPORTED)
        get_target_property(_token_type "${_real}" TYPE)
        set(_imported_json "false")
        if(_imported)
            set(_imported_json "true")
        endif()
        string(APPEND _resolution "${_rsep}\n    \"${_token}\": {")
        string(APPEND _resolution "\"target\": \"${_real}\", ")
        string(APPEND _resolution "\"type\": \"${_token_type}\", ")
        string(APPEND _resolution "\"imported\": ${_imported_json}}")
        set(_rsep ",")
    endforeach()

    list(REMOVE_DUPLICATES _out_of_scope)
    list(SORT _out_of_scope)
    set(_out_of_scope_json "[]")
    if(_out_of_scope)
        string(REPLACE ";" "\",\"" _out_of_scope_json "${_out_of_scope}")
        set(_out_of_scope_json "[\"${_out_of_scope_json}\"]")
    endif()

    set(_facts "{")
    string(APPEND _facts "\n  \"facts_schema\": 1,")
    string(APPEND _facts "\n  \"source_dir\": \"${CMAKE_SOURCE_DIR}\",")
    string(APPEND _facts "\n  \"binary_dir\": \"${CMAKE_BINARY_DIR}\",")
    string(APPEND _facts "\n  \"directory_scoped_imported_targets\": ${_out_of_scope_json},")
    string(APPEND _facts "\n  \"system_name\": \"${CMAKE_SYSTEM_NAME}\",")
    string(APPEND _facts "\n  \"system_processor\": \"${CMAKE_SYSTEM_PROCESSOR}\",")
    string(APPEND _facts "\n  \"build_type\": \"${CMAKE_BUILD_TYPE}\",")
    string(APPEND _facts "\n  \"cmake_version\": \"${CMAKE_VERSION}\",")
    string(APPEND _facts "\n  \"features\": {")
    set(_fsep "")
    foreach(_feature IN LISTS _feature_vars)
        set(_value "OFF")
        if(${_feature})
            set(_value "ON")
        endif()
        string(APPEND _facts "${_fsep}\n    \"${_feature}\": \"${_value}\"")
        set(_fsep ",")
    endforeach()
    string(APPEND _facts "\n  },")
    string(APPEND _facts "\n  \"name_resolution\": {${_resolution}\n  },")
    string(APPEND _facts "\n  \"targets\": {${_body}\n  }")
    string(APPEND _facts "\n}\n")
    file(WRITE "${CMAKE_BINARY_DIR}/consumption-census-facts.json" "${_facts}")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _pulp_census_write_facts)

# ── drift gate ─────────────────────────────────────────────────────────────
#
# docs/status/consumption-profiles.json is generated, so the only thing that
# keeps it true is a test that regenerates it and compares. These read the
# facts dump the block above writes; none of them configure or build anything.

add_test(NAME consumption-census-schema
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/consumption_census.py"
        --validate-only
        --source-root "${CMAKE_SOURCE_DIR}")
set_tests_properties(consumption-census-schema PROPERTIES
    PASS_REGULAR_EXPRESSION "consumption_census_schema_valid=true")

# Closures depend on the platform and on the feature switches recorded in the
# census, so a build tree that is a different profile is not a failure and must
# not be reported as a pass either: the generator exits 77 and this SKIPs.
add_test(NAME consumption-census-drift
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/consumption_census.py"
        --check
        --build-dir "${CMAKE_BINARY_DIR}"
        --source-root "${CMAKE_SOURCE_DIR}")
set_tests_properties(consumption-census-drift PROPERTIES
    SKIP_RETURN_CODE 77
    PASS_REGULAR_EXPRESSION "consumption_census_verified=true")

add_test(NAME consumption-census-negative-contract
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/consumption_census_contract.py"
        --build-dir "${CMAKE_BINARY_DIR}"
        --repo-root "${CMAKE_SOURCE_DIR}")
set_tests_properties(consumption-census-negative-contract PROPERTIES
    SKIP_RETURN_CODE 77
    PASS_REGULAR_EXPRESSION
        "consumption_census_contract_case=valid-current.*consumption_census_contract_case=census-missing.*consumption_census_contract_case=closure-count-drift.*consumption_census_contract_case=exported-target-removed.*consumption_census_contract_case=exported-target-added.*consumption_census_contract_case=schema-violation.*consumption_census_contract_case=profile-not-recorded.*consumption_census_contract_case=profile-feature-mismatch.*consumption_census_contract_case=foreign-profile-ignored.*consumption_census_contract_case=facts-missing.*consumption_census_contract_case=unknown-generator-expression.*consumption_census_contract_case=export-without-target.*consumption_census_contract_verified=true")
