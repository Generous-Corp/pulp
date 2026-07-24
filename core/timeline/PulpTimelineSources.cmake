include_guard(GLOBAL)

function(pulp_resolve_timeline_sources root output)
    set(_PULP_TIMELINE_PORTABLE_SOURCE_FILES
        asset_schema_migrations.cpp
        assets.cpp
        automation_curve.cpp
        automation_document_internal.cpp
        automation_lane.cpp
        command.cpp
        document_session.cpp
        identity_directory.cpp
        id_remap.cpp
        journal.cpp
        json_span_reader.cpp
        model.cpp
        transaction.cpp
        transaction_automation_internal.cpp
        transaction_take_internal.cpp
        transaction_track_state_internal.cpp
        transaction_reduction_support.cpp
        undo.cpp
        schema_codegen.cpp
        schema_json.cpp
        schema_json_canonical.cpp
        schema_json_parser.cpp
        schema_json_preflight.cpp
        schema_release.cpp
        schema_json_validation.cpp
        schema_registry.cpp
        serialize_asset_loop_decode.cpp
        serialize_automation_decode.cpp
        serialize_decode_support.cpp
        serialize_decode.cpp
        serialize_project_decode.cpp
        serialize_command_decode.cpp
        serialize_encode.cpp
        serialize_release.cpp
        snapshot_equivalence.cpp
        structural_registry_validation.cpp
        track.cpp
        track_schema_migrations.cpp
        take_lane_schema_migrations.cpp
        take_lane.cpp
    )
    set(_sources)
    foreach(_source IN LISTS _PULP_TIMELINE_PORTABLE_SOURCE_FILES)
        list(APPEND _sources "${root}/core/timeline/src/${_source}")
    endforeach()
    set(${output} ${_sources} PARENT_SCOPE)
endfunction()

function(pulp_resolve_timeline_native_sources root output)
    set(_PULP_TIMELINE_NATIVE_SOURCE_FILES
        file_journal_codec.cpp
        file_journal_native_io.cpp
        file_journal.cpp
    )
    set(_sources)
    foreach(_source IN LISTS _PULP_TIMELINE_NATIVE_SOURCE_FILES)
        list(APPEND _sources "${root}/core/timeline/native/${_source}")
    endforeach()
    set(${output} ${_sources} PARENT_SCOPE)
endfunction()
