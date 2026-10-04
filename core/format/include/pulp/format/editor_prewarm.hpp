#pragma once

// Editor prewarm at instantiation: the format-core half.
//
// Format adapters call request_editor_prewarm() right after a host has
// instantiated the plug-in's Processor. It asks the processor what its editor
// evaluates on every open (Processor::editor_prewarm()) and hands that to the
// scheduler the view layer installs, which compiles and verifies it on a
// background worker (view::prewarm_scripted_ui). Format-core cannot link the
// view layer, so the scheduler is a function pointer: until a view-capable
// target installs one (pulp-format-view does, at static initialization), a
// request is a no-op.

#include <pulp/format/processor.hpp>

namespace pulp::format {

using EditorPrewarmScheduler = void (*)(const Processor::EditorPrewarm&);

/// Installed by the view layer. Passing nullptr uninstalls (tests).
void set_editor_prewarm_scheduler(EditorPrewarmScheduler scheduler) noexcept;

/// Called by format adapters once a host has instantiated `processor`. Does
/// nothing when the processor has no editor or nothing to prewarm, when the
/// environment blocks editors (headless, CI, PULP_DISABLE_PLUGIN_EDITOR),
/// when PULP_EDITOR_PREWARM=0, when no scheduler is installed, or when this
/// plug-in bundle (descriptor bundle id + name) was already requested in this
/// process. Returns true when it handed a request to the scheduler.
bool request_editor_prewarm(const Processor& processor);

namespace detail {
/// Forget which bundles were requested (tests only).
void reset_editor_prewarm_requests_for_tests();
}  // namespace detail

}  // namespace pulp::format
