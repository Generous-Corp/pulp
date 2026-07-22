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
    src/transaction_automation_internal.cpp
    src/transaction_reduction_support.cpp
    src/undo.cpp
    src/schema_json.cpp
    src/schema_json_canonical.cpp
    src/schema_json_parser.cpp
    src/schema_json_preflight.cpp
    src/schema_registry.cpp
    src/serialize_automation_decode.cpp
    src/serialize_decode.cpp
    src/serialize_encode.cpp
    src/snapshot_equivalence.cpp
    src/structural_registry_validation.cpp
    src/track.cpp
    src/track_schema_migrations.cpp
    # Foreign-format interop lives outside src/ on purpose: the web
    # timeline-source-closure gate sweeps every core/timeline/src/*.cpp into the
    # WAM/WebCLAP wasm DSP lanes, which do not link pulp-runtime. The DAWproject
    # importer depends on pugixml (via runtime) and is a native authoring/import
    # concern, so keeping it in import/ keeps pugixml out of the wasm plugin.
    import/dawproject_import.cpp
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

# The DAWproject importer parses project.xml with pugixml directly (its object
# code is already linked in via pulp::runtime). The header is an internal detail
# of runtime's XmlDocument, so the include path is added here privately rather
# than re-exported.
target_include_directories(pulp-timeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../external/pugixml)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-timeline PRIVATE -fno-exceptions -fno-rtti)
endif()
