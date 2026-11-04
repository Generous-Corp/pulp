#pragma once

/// @file editor_package.hpp
/// The default editor a plug-in binary declares with
/// `pulp_add_plugin(... UI <package>)`.
///
/// A UI package is a CMake target that names one function returning its
/// `EditorFactory`. For every format and standalone target of a plug-in that
/// declares `UI <package>`, `pulp_add_plugin` generates a small source into
/// that target. The source defines the hook, a function with internal linkage
/// that calls the package's function, and installs it here at static init.
/// `Processor::editor_factory()` returns what the installed hook returns, so a
/// processor that does not override `editor_factory()` gets its plug-in's
/// package editor, and `core/format` never names a UI package.
///
/// The slot is local to the image it is linked into. `pulp-format-core` is
/// linked statically into each plug-in module and executable, the slot has
/// internal linkage, and the two functions below have hidden visibility, so
/// two plug-ins loaded into one host process each read their own hook and
/// cannot replace each other's. `UI none`, or no `UI` argument, generates
/// nothing and links nothing: the slot stays empty and `editor_factory()`
/// returns null, as for a headless processor.
///
/// UI package convention (see docs/reference/cmake-functions.md):
/// - `PULP_EDITOR_FACTORY_FUNCTION`: the fully qualified name of a function
///   `::pulp::format::EditorFactory* name()` that returns the package's
///   factory. The factory must outlive every editor it creates; a
///   function-local static is the usual shape. The function is called on the
///   host's main thread, the first time a host asks for the editor.
/// - `PULP_EDITOR_FACTORY_HEADER`: the header, on the package's include path,
///   that declares that function.

namespace pulp::format {

class EditorFactory;

/// Returns the editor factory of the UI package a plug-in binary declares.
using EditorFactoryHook = EditorFactory* (*)();

#if defined(_WIN32)
#define PULP_FORMAT_IMAGE_LOCAL
#else
#define PULP_FORMAT_IMAGE_LOCAL __attribute__((visibility("hidden")))
#endif

/// Install this image's editor-factory hook. The source that
/// `pulp_add_plugin(... UI <package>)` generates calls this at static init.
/// Static-init safe; a later call replaces the hook, and null clears it.
PULP_FORMAT_IMAGE_LOCAL void install_plugin_editor_factory_hook(EditorFactoryHook hook) noexcept;

/// The editor factory the installed hook returns, or null when this image
/// installed none.
PULP_FORMAT_IMAGE_LOCAL EditorFactory* plugin_editor_factory();

} // namespace pulp::format
