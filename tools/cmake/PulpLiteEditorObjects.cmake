# PulpLiteEditorObjects.cmake — the lite editor stack's replacement objects.
#
# A plug-in whose editor never needs the GPU (AutoUi, or a C++ editor that
# paints on the CoreGraphics host) still linked Skia, Dawn and Tint, because a
# handful of library functions reach them unconditionally: TextShaper measures
# through SkParagraph, FontResolver asks Skia's font manager, CanvasWidget can
# draw a Dawn texture, and AssetManager registers a Skia emoji fallback. Each
# of those translation units is compiled a second time here with
# PULP_LITE_EDITOR=1, which swaps the Skia body for the CoreText or no-op one.
#
# A lite plug-in lists these objects in its own sources. Objects on the link
# line take precedence over archive members, so the library's Skia versions are
# never loaded and dead-strip removes everything they reached. If anything
# still references a symbol only the full member defines (a function the lite
# object leaves out, or code outside these four files that reaches Skia), the
# linker loads that member as well. ld64 reports the overlap only as
# "duplicate symbol" warnings and links, so the failure is the editor-stack
# check: check_plugin_module_editor_stack.cmake finds Skia in the module, and
# -Wl,-why_live,<symbol> on the module link names the chain.
#
# PULP_LITE_EDITOR may change function bodies only, never a declaration, so a
# lite object and the rest of the binary agree on every type.

include_guard(GLOBAL)

# _pulp_add_lite_editor_objects(<name> HOME <target> SOURCES <src>...)
#   Builds <src> as an OBJECT library with <target>'s own include directories,
#   definitions and options, its interface usage requirements, and
#   PULP_LITE_EDITOR=1.
function(_pulp_add_lite_editor_objects name)
    cmake_parse_arguments(LITE "" "HOME" "SOURCES" ${ARGN})
    if(NOT TARGET ${LITE_HOME})
        message(FATAL_ERROR "_pulp_add_lite_editor_objects(${name}): no target ${LITE_HOME}")
    endif()
    add_library(${name} OBJECT ${LITE_SOURCES})
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    get_target_property(_std ${LITE_HOME} CXX_STANDARD)
    if(_std)
        set_target_properties(${name} PROPERTIES CXX_STANDARD ${_std})
    endif()
    target_include_directories(${name} PRIVATE
        $<TARGET_PROPERTY:${LITE_HOME},INCLUDE_DIRECTORIES>)
    target_compile_definitions(${name} PRIVATE
        $<TARGET_PROPERTY:${LITE_HOME},COMPILE_DEFINITIONS>
        PULP_LITE_EDITOR=1)
    target_compile_options(${name} PRIVATE
        $<TARGET_PROPERTY:${LITE_HOME},COMPILE_OPTIONS>)
    # The home target's interface usage requirements (its PUBLIC dependencies);
    # its private ones arrive through the copied properties above.
    target_link_libraries(${name} PRIVATE ${LITE_HOME})
endfunction()
