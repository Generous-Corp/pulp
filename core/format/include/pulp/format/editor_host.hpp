#pragma once

/// @file editor_host.hpp
/// The editor-host contract: how a host (a format adapter, the standalone app,
/// a test) opens, sizes, feeds and tears down a processor's editor without
/// knowing what UI toolkit draws it.
///
/// The contract is deliberately small and UI-agnostic. It names no view,
/// canvas, render or GPU type, so a processor that ships an editor and one that
/// ships none compile against the same header, and any UI toolkit (a Pulp
/// integration package, or a plug-in's own native code) can implement it.
///
/// - `EditorFactory` builds an `Editor` for a processor, a host-supplied
///   `EditorContext` and a role. A processor exposes its factory through
///   `Processor::editor_factory()`; a processor with no factory has no editor
///   on this path. A plug-in's default editor is wired per plug-in, not by a
///   process-wide registration: `pulp_add_plugin(... UI <package>)` generates
///   a hook in that plug-in's own binary that returns the UI package's factory,
///   the way a VST3 plug-in supplies its own view from `createView()`. Several
///   plug-ins in one host process therefore never share or override each
///   other's editor choice.
/// - `Editor` is one editor instance in one native window: attach and detach,
///   size negotiation, a main-thread tick, synchronous key offering, an
///   optional automation provider, and host-driven invalidation.
/// - `EditorContext` is what the host gives the editor: the parameter port, the
///   visual port, a resize request, host actions, the host kind, and
///   `query_extension()` for capabilities added later.
/// - `ParamPort` is parameter access (info, get, begin, set, end, listeners).
///   `StateStoreParamPort` is the default implementation, routed through the
///   StateStore gesture path the format adapters already report to the host.
/// - `VisualPort` is read access to the lock-free values and meters the audio
///   thread publishes for the UI.
///
/// Threading: unless a member says otherwise, every call in this contract runs
/// on the host's main (UI) thread, including construction and destruction of
/// editors. `ParamPort::get()` and `ParamPort::info()` may be called from any
/// thread.

#include <pulp/format/editor_host_kind.hpp>
#include <pulp/format/plugin_descriptor.hpp>
#include <pulp/state/parameter.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::state {
class StateStore;
}

namespace pulp::format {

class Processor;

// ── Parameters ───────────────────────────────────────────────────────────────

/// Owns one `ParamPort` listener registration. Destroying it, or calling
/// `reset()`, removes the listener. Move-only.
class ParamSubscription {
  public:
    ParamSubscription() = default;
    /// Adopt a registration; `cancel` runs exactly once, on reset or
    /// destruction. Port implementations construct these.
    explicit ParamSubscription(std::function<void()> cancel) noexcept
        : cancel_(std::move(cancel)) {}
    ParamSubscription(ParamSubscription&& other) noexcept
        : cancel_(std::exchange(other.cancel_, {})) {}
    ParamSubscription& operator=(ParamSubscription&& other) noexcept {
        if (this != &other) {
            reset();
            cancel_ = std::exchange(other.cancel_, {});
        }
        return *this;
    }
    ParamSubscription(const ParamSubscription&) = delete;
    ParamSubscription& operator=(const ParamSubscription&) = delete;
    ~ParamSubscription() {
        reset();
    }

    /// Remove the listener now. Idempotent.
    void reset() noexcept {
        if (auto cancel = std::exchange(cancel_, {}))
            cancel();
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(cancel_);
    }

  private:
    std::function<void()> cancel_;
};

/// Parameter access for an editor.
///
/// Values are in the parameter's plain domain (the `ParamInfo::range` units);
/// `get_normalized()` and `set_normalized()` convert through that range.
///
/// Gestures: a continuous edit (a knob drag, a fader move) is
/// `begin(id)` → `set(id, v)` one or more times → `end(id)`, which the host
/// records as one automation pass and one undo step. Every `begin` needs a
/// matching `end`; a `set` outside an open gesture is a one-shot edit that the
/// port brackets with its own begin and end. Several editors may hold the same
/// parameter open at once: the host sees one begin when the first opens and one
/// end when the last closes.
///
/// Edits made through a port are editor edits. They reach the host through the
/// format adapter's existing gesture and value reporting
/// (`HostParameterEditBridge`), so a port needs no host-specific code.
class ParamPort {
  public:
    using Listener = std::function<void(state::ParamID id, float plain_value)>;

    virtual ~ParamPort() = default;

    /// Every parameter, in declaration order.
    virtual std::span<const state::ParamInfo> params() const = 0;
    /// One parameter's description, or null for an unknown id. Any thread.
    virtual const state::ParamInfo* info(state::ParamID id) const = 0;
    /// The current plain value (the host's automation included). Any thread.
    virtual float get(state::ParamID id) const = 0;

    /// Open a gesture on `id`.
    virtual void begin(state::ParamID id) = 0;
    /// Write a plain value. Inside a gesture this is one step of it; outside,
    /// a complete one-shot edit.
    virtual void set(state::ParamID id, float plain_value) = 0;
    /// Close a gesture this caller opened. An `end` without a matching `begin`
    /// is ignored.
    virtual void end(state::ParamID id) = 0;

    /// Be told about every change to any parameter, whoever made it (this
    /// editor, another editor, host automation, a state restore). Delivered on
    /// the main thread with the plain value; changes from the audio thread are
    /// coalesced to the latest value.
    [[nodiscard]] virtual ParamSubscription add_listener(Listener listener) = 0;

    /// `get(id)` mapped to [0, 1] through the parameter's range (0 for an
    /// unknown id).
    float get_normalized(state::ParamID id) const {
        const auto* i = info(id);
        return i ? i->range.normalize(get(id)) : 0.0f;
    }
    /// `set(id, …)` from a [0, 1] value through the parameter's range. Does
    /// nothing for an unknown id.
    void set_normalized(state::ParamID id, float normalized) {
        if (const auto* i = info(id))
            set(id, i->range.denormalize(normalized));
    }
};

/// The default `ParamPort`, over a processor's `StateStore`.
///
/// Gestures use the store's shared gesture leases
/// (`StateStore::acquire_gesture` / `release_gesture`), so any number of ports
/// and editors over one store open and close the host gesture exactly once per
/// overlapping edit. Writes use `StateStore::set_value`, the path the format
/// adapters already observe and report to the host as editor edits.
///
/// One port may be shared by several editors. Giving each editor its own port
/// over the same store also isolates teardown: destroying a port (or calling
/// `release_all()`) closes only the gestures that port opened, so closing one
/// window mid-drag never ends another window's gesture.
///
/// Main thread only, apart from `info()`, `params()`, `get()` and
/// `disconnect()`. The store must outlive the port or be disconnected from it
/// first: after `disconnect()` returns the port holds no reference to the
/// store and calls into it no more, so an editor that outlives the store (its
/// plug-in instance disposed while a host still holds the editor's window)
/// never reaches it. `get()`, `begin()`, `set()`, `end()` and
/// `add_listener()` are then refused, and the port's subscriptions are
/// already removed.
///
/// `params()` and `info()` describe the parameters as they were when the port
/// was made. The span and pointers they return stay valid for the port's
/// lifetime, connected or not, so an editor may keep them.
class StateStoreParamPort final : public ParamPort {
  public:
    explicit StateStoreParamPort(state::StateStore& store);
    ~StateStoreParamPort() override;

    StateStoreParamPort(const StateStoreParamPort&) = delete;
    StateStoreParamPort& operator=(const StateStoreParamPort&) = delete;

    std::span<const state::ParamInfo> params() const override;
    const state::ParamInfo* info(state::ParamID id) const override;
    float get(state::ParamID id) const override;
    void begin(state::ParamID id) override;
    void set(state::ParamID id, float plain_value) override;
    void end(state::ParamID id) override;
    [[nodiscard]] ParamSubscription add_listener(Listener listener) override;

    /// Close every gesture this port still holds, reporting each end to the
    /// host. Called by the destructor; a host calls it when the editor using
    /// the port goes away mid-gesture.
    void release_all();

    /// Release every gesture, remove every listener this port registered, and
    /// let go of the store, which may be destroyed once this returns. Waits
    /// for a store call already in progress on another thread to finish; the
    /// port never calls the store while holding its own lock. Any thread;
    /// idempotent.
    void disconnect();
    /// False after `disconnect()`.
    bool connected() const;

    /// Gestures this port currently holds open on `id` (0 when none).
    int open_gestures(state::ParamID id) const;

  private:
    struct Link;
    std::shared_ptr<Link> link_;
};

// ── Visual data ──────────────────────────────────────────────────────────────

/// The shape of one visual channel.
enum class VisualChannelShape : std::uint8_t {
    scalar = 0, ///< one number per publish (gain reduction, an envelope level)
    meter = 1,  ///< per-channel RMS and peak levels
    vector = 2, ///< a block of samples for a scope-style display
    events = 3, ///< a bounded list of occurrences from one audio block
};

/// What a visual channel is. The strings stay valid for the life of the port.
struct VisualChannelInfo {
    std::string_view name; ///< verbatim lookup key
    std::string_view unit; ///< display only ("dB", "Hz"); may be empty
    VisualChannelShape shape = VisualChannelShape::scalar;
    /// The value a reader falls back to when the channel goes stale.
    float neutral = 0.0f;
};

/// One occurrence on an events channel.
struct VisualEvent {
    std::uint32_t frame_index = 0; ///< sample offset inside its audio block
    float value = 0.0f;            ///< payload; a zero value is still an event
};

/// Read access to the values and meters a processor publishes for its UI.
///
/// The audio thread publishes, lock-free; this port reads the latest value of
/// each channel. Every channel is latest-wins: a reader that falls behind sees
/// the newest publish, not a history. Reads are lock-free and allocation-free.
///
/// One reader thread: all reads through one `VisualPort` happen on the same
/// thread (normally the editor's frame tick). The channel set is fixed before
/// audio runs, so an index resolved once stays valid.
class VisualPort {
  public:
    virtual ~VisualPort() = default;

    virtual std::size_t channel_count() const noexcept = 0;
    /// Description of channel `index` (< `channel_count()`).
    virtual VisualChannelInfo channel(std::size_t index) const noexcept = 0;

    /// Latest value of a scalar channel. False if `index` is not a scalar
    /// channel or nothing has been published yet.
    virtual bool read_scalar(std::size_t index, float& value) noexcept = 0;
    /// Latest frame of a meter channel into `rms` and `peak`. Returns the
    /// number of audio channels the frame carries (possibly more than the
    /// spans hold, which are filled up to their size), or 0 if `index` is not
    /// a meter channel.
    virtual std::size_t read_meter(std::size_t index, std::span<float> rms,
                                   std::span<float> peak) noexcept = 0;
    /// Latest block of a vector channel into `samples`. Returns the number of
    /// samples written, or 0 if `index` is not a vector channel.
    virtual std::size_t read_vector(std::size_t index, std::span<float> samples) noexcept = 0;
    /// Occurrences of the latest published block of an events channel. Returns
    /// the number written, or 0 if `index` is not an events channel.
    virtual std::size_t read_events(std::size_t index, std::span<VisualEvent> events) noexcept = 0;
    /// Monotonic publish count of channel `index`. A reader compares it across
    /// ticks to tell "the writer stopped" from "the value held steady".
    virtual std::uint32_t publish_count(std::size_t index) const noexcept = 0;

    /// Index of the channel called `name` with shape `shape`, or nullopt.
    std::optional<std::size_t> find(std::string_view name,
                                    VisualChannelShape shape) const noexcept {
        for (std::size_t i = 0, n = channel_count(); i < n; ++i) {
            const auto c = channel(i);
            if (c.shape == shape && c.name == name)
                return i;
        }
        return std::nullopt;
    }
};

// ── Native windows, sizes and keys ───────────────────────────────────────────

/// The UI role an editor plays. A processor may offer several at once.
enum class EditorRole : std::uint8_t {
    editor = 0,    ///< the main plug-in or app editor
    inspector = 1, ///< a secondary window over the same processor
    settings = 2,  ///< the processor's own settings content
};

/// The kind of native window handle an editor is attached to. Values are
/// stable; new kinds are only ever appended.
enum class NativeParentKind : std::uint8_t {
    none = 0,         ///< no window: an offscreen or headless editor
    cocoa_view = 1,   ///< macOS `NSView*` in `handle`
    uikit_view = 2,   ///< iOS `UIView*` in `handle`
    win32_window = 3, ///< Windows `HWND` in `handle`
    x11_window = 4,   ///< X11 `Window` id in `window_id`
};

/// A native window to attach an editor to, owned by the host.
struct NativeParent {
    NativeParentKind kind = NativeParentKind::none;
    void* handle = nullptr;      ///< pointer handle (`NSView*`, `UIView*`, `HWND`)
    std::uint64_t window_id = 0; ///< integer handle (an X11 `Window`)
};

/// An editor size in logical pixels, with the backing scale factor that maps
/// them to physical pixels (2.0 on a Retina display).
struct EditorSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double scale = 1.0;
};

/// A non-character key. Character keys arrive in `EditorKeyEvent::character`;
/// Space arrives as both `space` and `U' '`. Values are stable; new keys are
/// only ever appended.
enum class EditorKey : std::uint16_t {
    none = 0,
    space = 1,
    enter = 2,
    tab = 3,
    backspace = 4,
    escape = 5,
    delete_forward = 6,
    left = 7,
    right = 8,
    up = 9,
    down = 10,
    home = 11,
    end = 12,
    page_up = 13,
    page_down = 14,
};

/// Modifier bits in `EditorKeyEvent::modifiers`.
enum EditorKeyModifier : std::uint8_t {
    editor_modifier_shift = 1u << 0,
    editor_modifier_control = 1u << 1,
    editor_modifier_alt = 1u << 2,     ///< Option on macOS
    editor_modifier_command = 1u << 3, ///< Command on macOS, Windows key elsewhere
};

/// A key the host offers to the editor before acting on it itself.
struct EditorKeyEvent {
    EditorKey key = EditorKey::none;
    char32_t character = 0;     ///< composed Unicode scalar, 0 when none
    std::uint8_t modifiers = 0; ///< `EditorKeyModifier` bits
    bool is_down = true;        ///< false for a key release
    bool is_repeat = false;     ///< an auto-repeat of a held key
};

/// The editor's answer to an offered key.
enum class KeyDisposition : std::uint8_t {
    forward = 0,  ///< not the editor's: the host acts on it (Space → transport)
    consumed = 1, ///< the editor handled it; the host must not act on it
};

// ── Editor ───────────────────────────────────────────────────────────────────

/// Optional automation surface of an editor, so tooling (the `pulp control`
/// UI tools, tests) can see and drive any editor whatever draws it. Requests
/// and replies are JSON text. Each member reports false when the editor does
/// not support it. Main thread.
class EditorAutomation {
  public:
    virtual ~EditorAutomation() = default;

    /// Encode the editor's current frame as PNG into `png`.
    virtual bool capture_png(std::vector<std::uint8_t>& /*png*/) {
        return false;
    }
    /// Apply one synthetic input request (a click, drag, key or text entry
    /// described as JSON). On failure `error` says why.
    virtual bool inject_input(std::string_view /*request_json*/, std::string& error) {
        error = "input injection is not supported by this editor";
        return false;
    }
    /// Answer an observation query (for example the element tree, or one
    /// element's state) with a JSON snapshot in `reply_json`.
    virtual bool observe(std::string_view /*query_json*/, std::string& /*reply_json*/) {
        return false;
    }
};

/// One editor instance in one native window.
///
/// Lifecycle: the factory creates it unattached. The size members may be
/// called before the first `attach()`: some hosts negotiate scale and size
/// first and only then hand over a window, and some defer attaching until the
/// host's size settles. The host then calls `attach()` with its window, any of
/// the members below, and `detach()`; the editor may be attached again or
/// destroyed. If the host's window is going away underneath the editor without
/// an orderly detach, the host calls `invalidate()` instead.
///
/// Every member runs on the host's main thread.
///
/// Reaching the plug-in: an editor reaches parameter and visual state only
/// through its `EditorContext` ports. Once the host has called `invalidate()`,
/// or has begun closing the editor because its plug-in instance is going away
/// (including a teardown off the main thread, where the instance is destroyed
/// before the editor's close runs on the main thread), the editor must not use
/// the `Processor&` it was created with: that processor may already be gone.
/// Its ports are disconnected by then and refuse every call, so an editor that
/// only uses its context stays safe.
class Editor {
  public:
    virtual ~Editor() = default;

    /// Whether this editor can attach to a parent of `kind`. A host asks before
    /// choosing a windowing API.
    virtual bool supports_parent(NativeParentKind kind) const = 0;

    /// Attach to and start drawing into `parent`. False if the editor cannot
    /// attach to it, in which case it stays unattached.
    virtual bool attach(const NativeParent& parent) = 0;

    /// Stop drawing and remove everything the editor added to its parent. The
    /// parent is still valid during the call. A no-op when not attached.
    virtual void detach() = 0;

    /// The host's window is already gone or about to be destroyed by the host
    /// itself. Release every reference to the parent without touching it and
    /// make no further host calls; the editor is detached afterwards and is
    /// normally destroyed next.
    virtual void invalidate() = 0;

    /// The editor's current size.
    virtual EditorSize size() const = 0;

    /// The host resized the window (or changed its scale). Returns false when
    /// the editor cannot take that size; the host then asks `adjust_size()`.
    virtual bool set_size(const EditorSize& size) = 0;

    /// Preferred, minimum and maximum size and the aspect ratio to hold. Equal
    /// minimum and maximum mean a fixed-size editor. The default reports the
    /// current size as preferred and no bounds.
    virtual ViewSize size_hints() const {
        const auto s = size();
        ViewSize hints;
        hints.preferred_width = s.width;
        hints.preferred_height = s.height;
        return hints;
    }

    /// The nearest size to `proposed` this editor accepts, for a host that
    /// negotiates during an interactive resize. Default: `proposed`.
    virtual EditorSize adjust_size(const EditorSize& proposed) const {
        return proposed;
    }

    /// Main-thread tick from the host (a timer or the window's frame clock).
    virtual void idle() {}

    /// The editor's window became visible or hidden without being detached.
    virtual void visibility_changed(bool /*visible*/) {}

    /// The editor's window gained or lost keyboard focus.
    virtual void focus_changed(bool /*focused*/) {}

    /// A key the host would otherwise act on itself, offered synchronously
    /// before it does. Return `consumed` only when the editor needs the key
    /// (a focused text field, an open popup); otherwise `forward` so the host
    /// keeps its shortcuts, Space for transport in particular.
    virtual KeyDisposition offer_key(const EditorKeyEvent& /*event*/) {
        return KeyDisposition::forward;
    }

    /// The host highlights or un-highlights a parameter's control (for
    /// example while it is automation-enabled). `color_rgb` is 0xRRGGBB.
    virtual void parameter_highlight(state::ParamID /*id*/, bool /*highlighted*/,
                                     std::uint32_t /*color_rgb*/) {}

    /// The editor's automation surface, or null when it has none. Owned by the
    /// editor and valid while the editor lives.
    virtual EditorAutomation* automation() {
        return nullptr;
    }
};

// ── Host context ─────────────────────────────────────────────────────────────

/// Actions an editor can ask its host to take. Values are stable; new actions
/// are only ever appended.
enum class HostAction : std::uint16_t {
    /// State outside the parameters changed (a loaded sample, an editor
    /// setting); the host should treat the project as modified.
    mark_state_dirty = 0,
    /// Show the host's context menu for parameter `param` at (`x`, `y`) in
    /// the editor's logical coordinates.
    parameter_context_menu = 1,
    /// Ask the host to close this editor's window.
    close_editor = 2,
};

/// One host action and its arguments.
struct HostActionRequest {
    HostAction action = HostAction::mark_state_dirty;
    state::ParamID param = 0;
    float x = 0.0f;
    float y = 0.0f;
};

/// What the host gives an editor. Implemented by the host; outlives every
/// editor created with it. Main thread.
class EditorContext {
  public:
    virtual ~EditorContext() = default;

    /// Parameter access. Several editors may share one port.
    virtual ParamPort& params() = 0;

    /// The processor's published values and meters, or null when it declares
    /// none.
    virtual VisualPort* visuals() = 0;

    /// Ask the host to resize the editor's window to `size`. True when the
    /// host accepted; the host then calls `Editor::set_size()`. False when the
    /// host refuses or cannot resize; the editor keeps its current size.
    virtual bool request_resize(const EditorSize& size) = 0;

    /// Ask the host to take `request`. False when the host does not support
    /// that action or declined it.
    virtual bool host_action(const HostActionRequest& request) = 0;

    /// Whether the editor runs inside a plug-in host or the standalone app.
    virtual EditorHostKind host_kind() const = 0;

    /// A host capability beyond the members above, by stable string id, or
    /// null when this host does not offer it.
    ///
    /// This is how the contract grows without breaking prebuilt editors or
    /// hosts: appending a virtual to this class would shift the vtable an
    /// editor built against an older header calls through, while a new
    /// capability added here is just a new id. The id names an interface
    /// documented next to it (a reverse-DNS string such as
    /// "dev.pulp.editor.example/1", versioned in the id itself); the returned
    /// pointer is that interface, owned by the host and valid while this
    /// context lives. An editor that does not know an id never asks for it,
    /// and a host that does not know one returns null.
    ///
    /// The GPU device is not reached this way: an editor acquires it from the
    /// shared GPU runtime with a role tag.
    virtual void* query_extension(std::string_view /*id*/) {
        return nullptr;
    }
};

// ── Factory ──────────────────────────────────────────────────────────────────

/// Builds a processor's editors. Owned by the processor
/// (`Processor::editor_factory()`) and alive as long as the processor.
class EditorFactory {
  public:
    virtual ~EditorFactory() = default;

    /// Whether this factory builds editors for `role`.
    virtual bool supports(EditorRole role) const = 0;

    /// A new, unattached editor for `processor` in `role`, or null when the
    /// role is not supported. `context` outlives the editor. Several editors
    /// may be alive at once, for one role or several. `processor` is valid
    /// only until the host invalidates the editor or begins closing it for a
    /// departing plug-in instance (see `Editor`); state after that is reached
    /// through `context`, whose ports refuse calls once disconnected.
    virtual std::unique_ptr<Editor> create(Processor& processor, EditorContext& context,
                                           EditorRole role) = 0;

    /// Warm process-wide caches (compiled scripts, parsed documents, fonts) so
    /// the first `create()` does not do that work inside the host's
    /// view-creation call. A host calls it at most once per factory, on a
    /// background thread, possibly while `create()` runs on the main thread;
    /// the factory synchronizes its own caches. It must not touch processor
    /// state or call the host. Default: nothing to warm.
    virtual void prewarm() {}
};

} // namespace pulp::format
