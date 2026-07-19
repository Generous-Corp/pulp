add_library(pulp-timeline
    src/assets.cpp
    src/automation_curve.cpp
    src/automation_lane.cpp
    src/command.cpp
    src/document_session.cpp
    src/identity_directory.cpp
    src/id_remap.cpp
    src/journal.cpp
    src/model.cpp
    src/transaction.cpp
    src/undo.cpp
    src/schema_json.cpp
    src/schema_json_canonical.cpp
    src/schema_json_parser.cpp
    src/schema_json_preflight.cpp
    src/schema_registry.cpp
    src/serialize_decode.cpp
    src/serialize_encode.cpp
    src/structural_registry_validation.cpp
    src/track.cpp
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
