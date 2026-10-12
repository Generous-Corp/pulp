# Design units: the DesignIR value model below view-core, the importer above it,
# and the Pulp-owned audio-control recognition pack above the importer.
#
#   pulp-design-ir                  header-only value types (DesignIR and the
#                                   design-source result/option structs). Standard
#                                   library only; view-core links it PUBLIC, so the
#                                   generic widgets and the widget bridge carry IR
#                                   values without reaching the importer.
#   pulp-design-import              importers, codegen, lock-to-source, JSX-lock
#                                   (PULP_ENABLE_DESIGN_IMPORT). Links view-core and
#                                   design-ir, never an audio, DSP, host, MIDI or
#                                   format target.
#   pulp-design-import-audio-pack   the built-in Pulp Figma Library of audio controls
#                                   for the generic recognition resolver. Product
#                                   data, so it is kept out of the importer.
#
# Files stay where they are; each target names its files explicitly. The header
# lists below are the units' public-header manifests: they are recorded on the
# targets as PULP_UNIT_HEADERS and checked, together with SOURCES and the link
# edges, by tools/scripts/design_unit_boundary_check.py.

set(PULP_DESIGN_IR_HEADERS
    include/pulp/view/design_ir.hpp
    include/pulp/view/design_source_types.hpp
)

# pulp-design-ir is declared in core/view/CMakeLists.txt, where the release
# artifact contract (tools/scripts/test_release_artifact_contents.py) reads
# INTERFACE declarations to tell header-only targets from archives.
target_include_directories(pulp-design-ir INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_property(TARGET pulp-design-ir PROPERTY PULP_UNIT_HEADERS ${PULP_DESIGN_IR_HEADERS})
target_link_libraries(pulp-view-core PUBLIC pulp::design-ir)

if(NOT PULP_ENABLE_DESIGN_IMPORT)
    return()
endif()

set(PULP_DESIGN_IMPORT_HEADERS
    include/pulp/view/anchor_strategy.hpp
    include/pulp/view/design_capture_lowering.hpp
    include/pulp/view/design_codegen.hpp
    include/pulp/view/design_export.hpp
    include/pulp/view/design_fidelity.hpp
    include/pulp/view/design_fidelity_ledger.hpp
    include/pulp/view/design_import.hpp
    include/pulp/view/design_ir_io.hpp
    include/pulp/view/design_shortcuts.hpp
    include/pulp/view/design_sources.hpp
    include/pulp/view/design_tokens.hpp
    include/pulp/view/design_tokens_w3c.hpp
    include/pulp/view/design_update.hpp
    include/pulp/view/jsx_lock.hpp
    include/pulp/view/lock_to_source.hpp
    include/pulp/view/recognition_resolver.hpp
    include/pulp/view/token_lock.hpp
    include/pulp/design/design_adherence.hpp
    include/pulp/design/design_gallery.hpp
    include/pulp/design/design_handoff.hpp
    include/pulp/design/design_knobs.hpp
    include/pulp/design/design_ledger.hpp
    include/pulp/design/design_manifest.hpp
    include/pulp/design/design_tweaks.hpp
    include/pulp/design/design_variants.hpp
)

add_library(pulp-design-import STATIC
    src/anchor_strategy.cpp
    src/design_export.cpp
    src/design_import.cpp
    src/design_update.cpp
    src/design_import_claude_css.cpp
    src/design_import_png.cpp
    src/design_binding_metadata.cpp
    src/design_ir_json.cpp
    src/design_ir_normalize.cpp
    src/recognition_resolver.cpp
    src/design_ir_analysis.cpp
    src/design_import_v0_tsx.cpp
    src/design_import_native_common.cpp
    src/design_import_legacy_registry.cpp
    src/design_import_document_checks.cpp
    src/design_cpp_codegen.cpp
    src/design_cpp_binding_codegen.cpp
    src/design_cpp_codegen_support.cpp
    src/design_cpp_style_codegen.cpp
    src/design_cpp_tree_codegen.cpp
    src/design_swift_codegen.cpp
    src/design_tokens.cpp
    src/design_tokens_w3c.cpp
    src/design_manifest.cpp
    src/design_adherence.cpp
    src/design_ledger.cpp
    src/design_codegen.cpp
    src/design_fidelity.cpp
    src/design_fidelity_ledger.cpp
    src/design_variants.cpp
    src/design_gallery.cpp
    src/design_handoff.cpp
    src/design_tweaks.cpp
    src/design_knobs.cpp
    src/lock_to_source.cpp
    src/jsx_lock.cpp
    src/design_import_shortcuts.cpp
    src/design_import_designmd.cpp
    src/design_import_designmd_schema.cpp
    src/token_lock.cpp
)
add_library(pulp::design-import ALIAS pulp-design-import)
set_property(TARGET pulp-design-import PROPERTY PULP_UNIT_HEADERS ${PULP_DESIGN_IMPORT_HEADERS})
# Native materialization binds imported data displays through the view-core
# facets; pulp-view-audio supplies concrete factories when it is registered.
target_link_libraries(pulp-design-import PUBLIC pulp::design-ir pulp::view-core)
# yaml-cpp (MIT) — DESIGN.md frontmatter parsing. Public Pulp headers expose no
# YAML types, but pulp-design-import is static: its parser object retains YAML
# symbols that the final consumer link must resolve. Keep yaml-cpp private in
# the API while preserving that static link closure in installed SDKs.
target_link_libraries(pulp-design-import PRIVATE
    $<BUILD_INTERFACE:yaml-cpp::yaml-cpp>
    $<INSTALL_INTERFACE:Pulp::yaml-cpp>
)

add_library(pulp-design-import-audio-pack STATIC src/recognition_audio_pack.cpp)
add_library(pulp::design-import-audio-pack ALIAS pulp-design-import-audio-pack)
set_property(TARGET pulp-design-import-audio-pack PROPERTY PULP_UNIT_HEADERS
    include/pulp/view/recognition_audio_pack.hpp)
target_link_libraries(pulp-design-import-audio-pack PUBLIC pulp::design-import)

# The runtime-import bridge compiled into view-script calls the importer, and
# the umbrellas keep handing every consumer the whole design-import surface
# (importer plus the built-in recognition pack), as they did when the importer
# lived inside view-core.
target_link_libraries(pulp-view-script PUBLIC pulp::design-import)
foreach(_pulp_view_target pulp-view pulp-view-native)
    target_link_libraries(${_pulp_view_target} PUBLIC pulp::design-import-audio-pack)
endforeach()
unset(_pulp_view_target)
