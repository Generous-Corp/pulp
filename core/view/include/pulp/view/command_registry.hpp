#pragma once

/// @file command_registry.hpp
/// CommandRegistry + CommandHandler + CommandInfo + ShortcutMap — Pulp-native
/// command dispatch and keyboard-shortcut routing.
///
/// Pulp-native naming per the license-lineage note in
/// `planning/2026-05-24-macos-plugin-authoring-plan.md` section 6.4: the
/// primitives are deliberately *not* named after any reference framework's
/// classes. The behavior contract is "a key event finds the topmost focused
/// handler that claims a command and dispatches to it".
///
/// ## Architecture
///
/// - `CommandID` — opaque 32-bit ID. Apps define their own constants.
/// - `CommandInfo` — metadata for one command (display name, category,
///   default shortcut, enabled state). Used by the `KeyMappingEditor`.
/// - `CommandHandler` — interface implemented by any object that knows how
///   to perform commands. Typically a View subclass, but doesn't have to
///   be — anything with command knowledge can register. The handler
///   reports which commands it can perform and runs them on demand.
/// - `CommandRegistry` — central per-app dispatcher. Owns:
///     * the set of known `CommandInfo` records;
///     * the chain of registered `CommandHandler` pointers (front of the
///       chain = highest priority);
///     * the `ShortcutMap` (key + modifier → command id).
///   `dispatch(id)` walks the handler chain top-to-bottom and invokes the
///   first handler that claims the command.
/// - `ShortcutMap` — pure data: associates a `(KeyCode, modifiers)` pair
///   with a `CommandID`. The registry uses it for routing; the editor UI
///   uses it for rebinding. Persists via `pulp::state::PropertiesFile`.
///
/// ## Routing through the existing focus chain
///
/// The registry exposes `dispatch_key_event(const KeyEvent&)` which:
///   1. Looks up the (key, modifiers) tuple in the `ShortcutMap`.
///   2. If no command matches, returns false (host can fall through to
///      its existing per-View `on_key_event` delivery).
///   3. If a command matches, walks the registered handler chain and
///      invokes the first one that claims the command. Returns true
///      iff a handler ran.
///
/// The contract intentionally lets the host integrate two ways:
///   (a) call `dispatch_key_event` BEFORE the normal `on_key_event`
///       delivery — global shortcuts win;
///   (b) call it AFTER — focused widget wins, with the registry as a
///       fall-through (the macOS-MVP plan's default).
///
/// ## Persistence
///
/// `ShortcutMap::save_to(PropertiesFile&)` and `load_from(PropertiesFile&)`
/// round-trip the user's rebindings through Pulp's
/// `pulp::state::ApplicationProperties` (user settings file). The editor UI
/// calls save() on every commit so changes survive a restart.

#include <pulp/view/input_events.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulp::state { class PropertiesFile; }

namespace pulp::view {

/// Opaque 32-bit command identifier. Apps define their own enum-style
/// constants (e.g. `constexpr CommandID kFileOpen = 0x1001`). Zero is
/// reserved for "no command".
using CommandID = std::uint32_t;

inline constexpr CommandID kInvalidCommandID = 0;

/// Metadata for one command. Used by `KeyMappingEditor` to render the
/// list of rebindable actions and by the registry for display/debug.
struct CommandInfo {
    CommandID id = kInvalidCommandID;
    std::string name;          ///< User-facing display name ("Open File…")
    std::string category;      ///< Group ("File", "Edit", "View") for the editor.
    KeyCode default_key = KeyCode::unknown;
    std::uint16_t default_modifiers = 0;
    bool enabled = true;       ///< False = greyed out / not dispatched.
};

/// Interface implemented by any object that can perform commands.
///
/// A handler is typically a View subclass but doesn't have to be — the
/// registry holds raw `CommandHandler*` and the owner is responsible for
/// removing the handler before destruction (see
/// `CommandRegistry::remove_handler`). This mirrors how Pulp's other
/// listener APIs (timer, async-update) work.
class CommandHandler {
public:
    virtual ~CommandHandler() = default;

    /// Report which commands this handler can perform. Called by the
    /// registry when dispatching; an empty result means "I don't know
    /// about that command — try the next handler". The default
    /// implementation returns the static `commands()` set, which makes
    /// per-instance overrides cheap.
    virtual std::vector<CommandID> commands() const = 0;

    /// Perform a command. Return true if handled (stops the dispatch
    /// walk), false to let the next handler in the chain try.
    virtual bool perform_command(CommandID id) = 0;
};

/// (KeyCode, modifiers) → CommandID lookup table. Pure data; safe to
/// copy. Persists through `save_to` / `load_from`.
class ShortcutMap {
public:
    /// Associate a (key, modifier) chord with a command. If a different
    /// command was previously bound to the same chord, it is replaced.
    void bind(KeyCode key, std::uint16_t modifiers, CommandID id);

    /// Remove any binding for the given chord. No-op if unbound.
    void unbind(KeyCode key, std::uint16_t modifiers);

    /// Remove every chord that points to the given command.
    void unbind_command(CommandID id);

    /// Look up a chord. Returns `kInvalidCommandID` if unbound.
    CommandID find(KeyCode key, std::uint16_t modifiers) const;

    /// All chords currently bound to the given command. Useful for the
    /// editor UI to show "command X is bound to Cmd-O, F2".
    struct Chord {
        KeyCode key = KeyCode::unknown;
        std::uint16_t modifiers = 0;
    };
    std::vector<Chord> chords_for(CommandID id) const;

    /// Number of bound chords.
    std::size_t size() const { return bindings_.size(); }

    /// Clear all bindings.
    void clear() { bindings_.clear(); }

    /// Serialize / restore through a `pulp::state::PropertiesFile`.
    /// Round-trip is lossless. The key namespace inside the properties
    /// file is `"shortcuts.<key>.<mods>"` → `<command-id>` so different
    /// subsystems can share one file without colliding (callers can
    /// pass a custom `key_prefix` to use their own namespace).
    void save_to(pulp::state::PropertiesFile& props,
                 std::string_view key_prefix = "shortcuts") const;
    bool load_from(const pulp::state::PropertiesFile& props,
                   std::string_view key_prefix = "shortcuts");

private:
    // Composite key: key code in low 32 bits, modifiers in high 16 bits.
    using ChordKey = std::uint64_t;
    static ChordKey make_key(KeyCode k, std::uint16_t m) {
        return (static_cast<ChordKey>(m) << 32) |
               static_cast<ChordKey>(static_cast<std::uint32_t>(k));
    }
    std::unordered_map<ChordKey, CommandID> bindings_;
};

/// Central command dispatcher. One per application (or per window, if you
/// want per-window scopes). Owns the `ShortcutMap` and a chain of
/// registered `CommandHandler` pointers.
class CommandRegistry {
public:
    CommandRegistry() = default;
    ~CommandRegistry() = default;

    // Non-copyable, non-movable — handlers register by raw pointer.
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    // ── Command metadata ────────────────────────────────────────────

    /// Register a command. If the info's `default_key` is non-unknown,
    /// the registry also binds the default chord in the `ShortcutMap`
    /// (unless that chord is already bound — user-loaded rebindings
    /// take priority, which is why apps should `load` the shortcut map
    /// from disk BEFORE calling `register_command`).
    void register_command(const CommandInfo& info);

    /// Look up a command by id. Returns nullopt if unknown.
    std::optional<CommandInfo> command_info(CommandID id) const;

    /// All registered commands, in registration order.
    const std::vector<CommandInfo>& commands() const { return commands_; }

    /// Mutate a command's `enabled` flag (e.g. greys out "Undo" when
    /// the stack is empty). Does not change shortcut bindings.
    void set_enabled(CommandID id, bool enabled);

    // ── Handler chain ───────────────────────────────────────────────

    /// Add a handler to the front of the dispatch chain (highest
    /// priority). Caller retains ownership; must call `remove_handler`
    /// before the handler is destroyed.
    void add_handler(CommandHandler* handler);

    /// Remove a handler from the chain. Safe to call with an unknown
    /// pointer (no-op).
    void remove_handler(CommandHandler* handler);

    /// Number of handlers currently in the chain.
    std::size_t handler_count() const { return handlers_.size(); }

    // ── Dispatch ────────────────────────────────────────────────────

    /// Dispatch a command id through the handler chain. Returns true if
    /// some handler claimed it. Commands marked `enabled=false` are not
    /// dispatched and the call returns false.
    bool dispatch(CommandID id);

    /// Dispatch a key event: look up the chord in the `ShortcutMap`,
    /// then dispatch the resulting command (if any). Returns true iff
    /// a command was found AND a handler claimed it.
    bool dispatch_key_event(const KeyEvent& event);

    // ── ShortcutMap access ──────────────────────────────────────────

    ShortcutMap& shortcuts() { return shortcuts_; }
    const ShortcutMap& shortcuts() const { return shortcuts_; }

private:
    std::vector<CommandInfo> commands_;
    std::unordered_map<CommandID, std::size_t> command_index_;
    std::vector<CommandHandler*> handlers_;
    ShortcutMap shortcuts_;
};

} // namespace pulp::view
