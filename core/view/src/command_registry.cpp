// command_registry.cpp — Pulp-native command dispatch + shortcut routing.
//
// See `core/view/include/pulp/view/command_registry.hpp` for the contract.
// Pulp-native names per planning/2026-05-24-macos-plugin-authoring-plan.md
// section 6.4 (license-lineage note).

#include <pulp/view/command_registry.hpp>
#include <pulp/state/properties_file.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace pulp::view {

// ── ShortcutMap ─────────────────────────────────────────────────────────

void ShortcutMap::bind(KeyCode key, std::uint16_t modifiers, CommandID id) {
    if (id == kInvalidCommandID) {
        unbind(key, modifiers);
        return;
    }
    bindings_[make_key(key, modifiers)] = id;
}

void ShortcutMap::unbind(KeyCode key, std::uint16_t modifiers) {
    bindings_.erase(make_key(key, modifiers));
}

void ShortcutMap::unbind_command(CommandID id) {
    for (auto it = bindings_.begin(); it != bindings_.end();) {
        if (it->second == id) it = bindings_.erase(it);
        else ++it;
    }
}

CommandID ShortcutMap::find(KeyCode key, std::uint16_t modifiers) const {
    auto it = bindings_.find(make_key(key, modifiers));
    return (it == bindings_.end()) ? kInvalidCommandID : it->second;
}

std::vector<ShortcutMap::Chord> ShortcutMap::chords_for(CommandID id) const {
    std::vector<Chord> result;
    for (auto& [k, v] : bindings_) {
        if (v != id) continue;
        Chord c;
        c.key = static_cast<KeyCode>(static_cast<std::uint32_t>(k & 0xFFFFFFFFu));
        c.modifiers = static_cast<std::uint16_t>((k >> 32) & 0xFFFFu);
        result.push_back(c);
    }
    return result;
}

void ShortcutMap::save_to(pulp::state::PropertiesFile& props,
                          std::string_view key_prefix) const {
    // Build a "<prefix>.<key>.<mods>" → "<command-id>" key for each
    // binding. Strip the existing keys for this prefix first so removed
    // bindings don't linger in the persisted file.
    const std::string prefix = std::string(key_prefix) + ".";
    {
        // PropertiesFile doesn't expose a prefix-remove, so iterate keys
        // and remove anything under our prefix. `keys()` returns a copy
        // so it's safe to mutate during the walk.
        for (const auto& k : props.keys()) {
            if (k.size() > prefix.size() &&
                std::equal(prefix.begin(), prefix.end(), k.begin())) {
                props.remove(k);
            }
        }
    }
    for (const auto& [chord_key, cmd] : bindings_) {
        auto key_code = static_cast<std::uint32_t>(chord_key & 0xFFFFFFFFu);
        auto mods = static_cast<std::uint16_t>((chord_key >> 32) & 0xFFFFu);
        std::string k = prefix + std::to_string(key_code) + "." +
                        std::to_string(static_cast<unsigned>(mods));
        props.set_int(k, static_cast<std::int64_t>(cmd));
    }
}

bool ShortcutMap::load_from(const pulp::state::PropertiesFile& props,
                            std::string_view key_prefix) {
    const std::string prefix = std::string(key_prefix) + ".";
    bool any = false;
    for (const auto& k : props.keys()) {
        if (k.size() <= prefix.size()) continue;
        if (!std::equal(prefix.begin(), prefix.end(), k.begin())) continue;
        // "<prefix>.<key>.<mods>" — find the two dots after prefix.
        auto dot1 = k.find('.', prefix.size());
        if (dot1 == std::string::npos) continue;
        std::string key_part = k.substr(prefix.size(), dot1 - prefix.size());
        std::string mods_part = k.substr(dot1 + 1);
        try {
            auto key_code = static_cast<std::uint32_t>(std::stoul(key_part));
            auto mods = static_cast<std::uint16_t>(std::stoul(mods_part));
            auto cmd = props.get_int(k);
            if (!cmd) continue;
            bindings_[make_key(static_cast<KeyCode>(key_code), mods)] =
                static_cast<CommandID>(*cmd);
            any = true;
        } catch (...) {
            // Malformed key — skip rather than reject the whole file.
            continue;
        }
    }
    return any;
}

// ── CommandRegistry ─────────────────────────────────────────────────────

void CommandRegistry::register_command(const CommandInfo& info) {
    if (info.id == kInvalidCommandID) return;
    auto it = command_index_.find(info.id);
    if (it != command_index_.end()) {
        commands_[it->second] = info;
    } else {
        command_index_[info.id] = commands_.size();
        commands_.push_back(info);
    }
    // Bind the default chord only if (a) nothing else already owns it AND
    // (b) this command has no existing binding. (a) protects another
    // command's chord; (b) lets apps load() the user's saved ShortcutMap
    // BEFORE register_command and keep the user's rebind even when the
    // default chord is free.
    if (info.default_key != KeyCode::unknown &&
        shortcuts_.find(info.default_key, info.default_modifiers) ==
            kInvalidCommandID &&
        shortcuts_.chords_for(info.id).empty()) {
        shortcuts_.bind(info.default_key, info.default_modifiers, info.id);
    }
}

std::optional<CommandInfo> CommandRegistry::command_info(CommandID id) const {
    auto it = command_index_.find(id);
    if (it == command_index_.end()) return std::nullopt;
    return commands_[it->second];
}

void CommandRegistry::set_enabled(CommandID id, bool enabled) {
    auto it = command_index_.find(id);
    if (it == command_index_.end()) return;
    commands_[it->second].enabled = enabled;
}

void CommandRegistry::add_handler(CommandHandler* handler) {
    if (!handler) return;
    // Push to the front: most-recently-added wins, matching how a
    // newly-opened modal/panel grabs shortcut priority.
    handlers_.insert(handlers_.begin(), handler);
}

void CommandRegistry::remove_handler(CommandHandler* handler) {
    if (!handler) return;
    handlers_.erase(std::remove(handlers_.begin(), handlers_.end(), handler),
                    handlers_.end());
}

bool CommandRegistry::dispatch(CommandID id) {
    if (id == kInvalidCommandID) return false;
    auto idx = command_index_.find(id);
    if (idx != command_index_.end() && !commands_[idx->second].enabled) {
        return false;
    }
    for (auto* handler : handlers_) {
        if (!handler) continue;
        auto known = handler->commands();
        if (std::find(known.begin(), known.end(), id) == known.end()) continue;
        if (handler->perform_command(id)) return true;
    }
    return false;
}

bool CommandRegistry::dispatch_key_event(const KeyEvent& event) {
    if (!event.is_down) return false; // commands fire on key-down only
    CommandID id = shortcuts_.find(event.key, event.modifiers);
    if (id == kInvalidCommandID) return false;
    return dispatch(id);
}

} // namespace pulp::view
