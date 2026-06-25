#pragma once

// Convenience header — includes the full Pulp view system
#include <pulp/view/geometry.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/waveform_gpu_primitives.hpp>
#include <pulp/view/waveform_gpu_render_controller.hpp>
#include <pulp/view/waveform_headless_render_backend.hpp>
// JS scripting layer (pulp::view-script). Gated on the PUBLIC PULP_ENABLE_JS
// compile definition from pulp-view-core: in a native-only build (OFF) these
// headers declare symbols that live in the EXCLUDE_FROM_ALL view-script archive
// and are not linked, so pulling them here would break a native-only consumer.
#if PULP_ENABLE_JS
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/hot_reload.hpp>
#endif
#include <pulp/view/audio_bridge.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
