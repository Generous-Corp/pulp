add_library(pulp-timeline
    src/assets.cpp
    src/automation_curve.cpp
    src/automation_document_internal.cpp
    src/automation_lane.cpp
    src/command.cpp
    src/document_session.cpp
    src/identity_directory.cpp
    src/id_remap.cpp
    src/journal.cpp
    src/model.cpp
    src/transaction.cpp
    src/transaction_annotation_internal.cpp
    src/undo.cpp
    src/schema_json.cpp
    src/schema_json_canonical.cpp
    src/schema_json_parser.cpp
    src/schema_json_preflight.cpp
    src/schema_registry.cpp
    src/sequence.cpp
    src/sequence_annotations.cpp
    src/sequence_schema_migrations.cpp
    src/serialize_automation_decode.cpp
    src/serialize_decode.cpp
    src/serialize_decode_support.cpp
    src/serialize_encode.cpp
    src/serialize_sequence_decode.cpp
    src/snapshot_equivalence.cpp
    src/structural_registry_validation.cpp
    src/track.cpp
    src/track_schema_migrations.cpp
    src/transaction_automation_internal.cpp
    src/transaction_reduction_support.cpp
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
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-timeline PRIVATE -fno-exceptions -fno-rtti)
endif()
