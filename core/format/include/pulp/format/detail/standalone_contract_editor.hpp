#pragma once

// The standalone app's editor for a processor that supplies an editor factory
// (`Processor::editor_factory()`, see pulp/format/editor_host.hpp).
//
// StandaloneApp::run_with_editor() takes this path whenever the factory is
// non-null, and keeps its create_view() window otherwise. The editor runs in
// its own native window through a ContractEditorSession: the window's content
// view is the editor's NativeParent, the window drives the idle tick, a window
// resize is Editor::set_size(), the window's backing scale is the editor's
// scale, and closing the window detaches the editor. A key the editor forwards
// goes to the host; Space is the standalone's transport key.
//
// Headless (StandaloneConfig::headless): the window is never shown. After the
// configured key sequence and frame delay, the editor's frame is captured
// through EditorAutomation::capture_png() into the screenshot path and the
// window closes. An editor that offers no capture fails the run rather than
// writing nothing.

#include <pulp/format/detail/standalone_key_sequence.hpp>

#include <functional>
#include <string>
#include <vector>

namespace pulp::state {
class StateStore;
}

namespace pulp::format {
class EditorFactory;
class Processor;
} // namespace pulp::format

namespace pulp::format::detail {

struct StandaloneContractEditorOptions {
    std::string title;
    bool headless = false;
    /// Required when headless: where the captured frame is written.
    std::string screenshot_path;
    int screenshot_frame_delay = 30;
    /// Keys delivered through the window's real key path, one every
    /// `key_frame_delay` frames, before the capture.
    std::vector<KeySequenceStep> key_steps;
    int key_frame_delay = 20;
    /// The host's transport: called for a Space the editor forwards.
    std::function<void()> on_transport_key;
    /// Called once per frame of the window's loop, on the UI thread.
    std::function<void()> on_idle;
};

/// True when this platform can host a contract editor in a native window.
bool standalone_contract_editor_supported();

/// Open the processor's contract editor in a native window and run until the
/// window closes (or, headless, until the frame is captured). False when the
/// editor cannot be opened or, headless, nothing could be captured.
bool run_standalone_contract_editor(Processor& processor, state::StateStore& store,
                                    EditorFactory& factory,
                                    const StandaloneContractEditorOptions& options);

} // namespace pulp::format::detail
