// plugin_key_routing.hpp — who consumes a key inside an embedded plugin editor.
//
// An embedded editor shares one keyboard with its DAW. The editor must consume
// the keys it genuinely handles (⌘Z, ⌘A, a typed character, an open menu's
// arrows) and hand back every key it does not, because the host owns transport
// (Space), Musical Typing, and its own accelerator table. Getting the first
// half right is easy and gets noticed immediately; getting the SECOND half
// wrong is what users report as "the plugin killed my spacebar".
//
// The answer must come from the view tree, not from a list of keys someone
// guessed at: `route_plugin_key` asks the overlay policy, then the focused
// view, then the root's global hook, and reports `forward_to_host` only when
// none of them claimed the key.
//
// This lives in core/view and takes no platform type, because the same
// question is asked from three different format seams that share no code:
// macOS `-keyDown:` / `-performKeyEquivalent:` (AU v2/v3 hosted NSView),
// `IPlugView::onKeyDown` (VST3), and CLAP's GUI extension. Each seam speaks a
// different consumed/not-consumed dialect; only the policy is shared.

#pragma once

#include <cstdint>

#include <pulp/view/input_events.hpp>

namespace pulp::view {

class View;

/// What an embedded plugin editor should do with a key it has been offered.
enum class PluginKeyDisposition : std::uint8_t {
    /// Nothing under the root claimed the key. The adapter must report
    /// "not handled" to the host so transport, Musical Typing, and the host's
    /// accelerator table keep working while the editor is open.
    forward_to_host,
    /// A view or the root's global hook consumed the key outright. The host
    /// must not also see it.
    consumed,
    /// A focused text field did not treat the key as a command, and the key
    /// is a plain printable candidate. The platform seam must run its own
    /// text-insertion path (AppKit `interpretKeyEvents:`, or a direct
    /// `on_text_input`) and then report the key as consumed. Distinct from
    /// `consumed` because only the platform owns dead keys and IME
    /// composition — core/view cannot synthesize that text itself.
    insert_as_text,
};

/// Platform-supplied facts about the key that core/view cannot derive from a
/// `KeyEvent` alone.
struct PluginKeyOffer {
    KeyEvent key;
    /// The key carries no printable character (a function key, or AppKit's
    /// 0xF700-0xF8FF private-use range). Such a key is never text, so a
    /// focused text field that declines it has genuinely not consumed it.
    bool is_function_key = false;
    /// Whether this call may offer the key to `root.on_global_key`.
    ///
    /// Off by default, because a seam that delivers one press TWICE would
    /// otherwise fire the hook — and the script `keydown` listener behind it —
    /// twice. macOS is exactly that seam: AppKit offers every key-down to
    /// `-performKeyEquivalent:` before `-keyDown:`, and that override owns the
    /// hook there. A seam with a single delivery point (VST3 `onKeyDown`) sets
    /// this, and is then the only place the hook is consulted for that press.
    bool offer_global_hook = false;
};

/// Resolve who consumes `offer` under `root`.
///
/// Order, and why it is this order:
///
///   1. Escape against an open overlay. A modal, an open dropdown, or a
///      declared popover owns Escape even when nothing holds focus — which is
///      the ordinary state while a popover is open — so this must precede the
///      focus gate or the key never reaches it. Delegates to
///      `route_escape_to_active_overlay`, whose contract decides what counts.
///   2. The focused view under `root`, and only under `root`: the focus slot
///      is process-global, so a second open editor's focused field must never
///      answer for this one. A view that accepts navigation input but not
///      text is offered only the bounded navigation set (see
///      `is_plugin_navigation_key`) — that floor keeps a focusable knob from
///      swallowing Space — and its own `on_key_event` return value decides
///      from there. A view that accepts text input is offered every key; a
///      chord or function key it declines cannot become text, so it falls
///      through to step 3 and, unclaimed there, to the host — rather than
///      being swallowed because a field happened to hold focus.
///   3. `root.on_global_key`, the framework-level hook a CommandRegistry or
///      the script bridge installs, when the seam asked for it
///      (`PluginKeyOffer::offer_global_hook`). It sees only what the focused
///      view declined, so an editor-wide ⌘Z does not fight a text field's own
///      undo.
///
/// Anything none of those claimed is `forward_to_host`. No allowlist of
/// "host keys" appears anywhere in the policy: forwarding is the DEFAULT, and
/// consumption is what has to be earned.
PluginKeyDisposition route_plugin_key(View& root, const PluginKeyOffer& offer);

/// The focused view under `root`, or nullptr.
///
/// `View::focused_input_` is process-global, so with two plugin editors open
/// it names whichever field the user last typed in — possibly the OTHER
/// editor's. Answering a key from that slot lets editor B consume a key aimed
/// at editor A and report it handled, so the host never sees it either. The
/// ancestor walk is what scopes the answer to this tree.
View* plugin_key_focus(View& root);

/// The bounded navigation set a non-text widget may borrow from the host.
///
/// Not a statement about which keys the host wants — the host wants every key
/// it is not denied. It is the floor that stops a merely navigable widget from
/// consuming transport: a focused knob or list holding an active navigation
/// interaction gets arrows/Home/End/Enter/Escape and nothing else, so Space
/// still starts the transport and letters still reach Musical Typing. Any
/// chord modifier removes a key from the set, because a chord belongs to the
/// host or to the global hook.
bool is_plugin_navigation_key(KeyCode key, std::uint16_t modifiers);

} // namespace pulp::view
