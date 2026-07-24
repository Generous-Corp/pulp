add_library(pulp-timeline
    src/asset_schema_migrations.cpp
    src/assets.cpp
    src/automation_curve.cpp
    src/automation_document_internal.cpp
    src/automation_lane.cpp
    src/command.cpp
    src/document_session.cpp
    src/identity_directory.cpp
    src/id_remap.cpp
    src/journal.cpp
    src/json_span_reader.cpp
    src/model.cpp
    src/transaction.cpp
    src/transaction_automation_internal.cpp
    src/transaction_take_internal.cpp
    src/transaction_track_state_internal.cpp
    src/transaction_reduction_support.cpp
    src/undo.cpp
    src/schema_codegen.cpp
    src/schema_json.cpp
    src/schema_json_canonical.cpp
    src/schema_json_parser.cpp
    src/schema_json_preflight.cpp
    src/schema_release.cpp
    src/schema_json_validation.cpp
    src/schema_registry.cpp
    src/serialize_asset_loop_decode.cpp
    src/serialize_automation_decode.cpp
    src/serialize_decode_support.cpp
    src/serialize_decode.cpp
    src/serialize_project_decode.cpp
    src/serialize_command_decode.cpp
    src/serialize_encode.cpp
    src/serialize_release.cpp
    src/snapshot_equivalence.cpp
    src/structural_registry_validation.cpp
    src/track.cpp
    src/track_schema_migrations.cpp
    src/take_lane_schema_migrations.cpp
    src/take_lane.cpp
    # Foreign-format interop lives outside src/ on purpose: the web
    # timeline-source-closure gate sweeps every core/timeline/src/*.cpp into the
    # WAM/WebCLAP wasm DSP lanes, which do not link pulp-runtime. The DAWproject
    # importer depends on pugixml (via runtime) and is a native authoring/import
    # concern, so keeping it in import/ keeps pugixml out of the wasm plugin.
    import/dawproject_import.cpp
    import/dawproject_import_support.cpp
    import/dawproject_media_sealer.cpp
    # Native crash-consistent storage is deliberately outside src/: browser
    # timeline builds consume the in-memory JournalSink seam but have no native
    # filesystem durability primitive.
    native/file_journal_codec.cpp
    native/file_journal_native_io.cpp
    native/file_journal.cpp
)
add_library(pulp::timeline ALIAS pulp-timeline)

target_compile_features(pulp-timeline PUBLIC cxx_std_20)

target_include_directories(pulp-timeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-timeline
    PUBLIC
        pulp::runtime
        pulp::timebase
    PRIVATE
        pulp::audio
)

# The DAWproject importer parses project.xml with pugixml directly (its object
# code is already linked in via pulp::runtime). The header is an internal detail
# of runtime's XmlDocument, so the include path is added here privately rather
# than re-exported.
target_include_directories(pulp-timeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pugixml)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-timeline PRIVATE -fno-exceptions -fno-rtti)
endif()

# Standalone generator for the schema-drift gate: emits the canonical
# JSON-Schema projection of the built-in registry (see tools/schema_drift_check.py).
add_executable(pulp-timeline-schema-emit tools/schema_emit_main.cpp)
target_link_libraries(pulp-timeline-schema-emit PRIVATE pulp::timeline)
