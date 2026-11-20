#pragma once

/// @file
/// The native editor host behind `vellum::view::PluginViewHost::create()`.
///
/// Vellum declares `PluginViewHost::create()` and its factory registry but no
/// Vellum library defines them: attaching an editor to a DAW's native parent
/// is plug-in embedding glue, which Pulp keeps. `pulp::vellum-host` defines
/// them. A factory registered with `PluginViewHost::set_factory()` wins;
/// otherwise `create()` returns the platform host below.
///
/// The macOS host is a layer-backed NSView. It paints the view tree through
/// Vellum's raster renderer (`render_to_rgba`) and routes mouse and wheel input
/// through Vellum's pointer delivery. It never opens a GPU surface: the
/// installed SDK does not expose one, so an editor that asks for the GPU host
/// (`Options::use_gpu`) gets this host and `is_gpu_backed()` reports false.
/// There is no keyboard or text input yet.

#include <vellum/view/plugin_view_host.hpp>

#include <memory>

namespace pulp::vellum::detail {

/// The platform host `PluginViewHost::create()` falls back to when no factory
/// is registered, or nullptr on a platform this target has no host for.
std::unique_ptr<::vellum::view::PluginViewHost>
make_platform_plugin_view_host(::vellum::view::View& root,
                               const ::vellum::view::PluginViewHost::Options& options);

/// True when this build carries a platform host (macOS today).
bool platform_plugin_view_host_available() noexcept;

} // namespace pulp::vellum::detail
