#pragma once

/// @file property_panel.hpp
/// PropertyPanel — typed property editor with Boolean / Choice / Slider
/// / Button / MultiChoice / Text variants and optional `PropertiesFile`
/// persistence (closes the gap-doc Phase 4 row
/// "PreferencesPanel + PropertyPanel widgets").
///
/// `PropertyPanel` is a sister widget to `PreferencesPanel`: while
/// PreferencesPanel manages "which page is shown", PropertyPanel hosts
/// the typed property *editors* on a page. Each property has a stable
/// `key`, a display `label`, a `kind` (BooleanToggle, Choice, Slider,
/// Button, MultiChoice, Text), and a typed value. The panel exposes:
///
///   - `add_*` builders for each typed variant,
///   - `value_for(key)` / `set_value(key, ...)` typed getters/setters,
///   - `simulate_*` test helpers (toggle, choose, slide, click,
///     multi-check, set-text) that fire the same logic a real UI event
///     would,
///   - `bind_persistence(PropertiesFile*)` to read all current values
///     from a `PropertiesFile` lane (one-shot at bind time) and to
///     write every subsequent change back under each property's key.
///
/// The persistence keys are the property keys verbatim — callers
/// typically pass a "section.field" form (e.g. `"audio.input_gain"`).
/// `bind_persistence` is idempotent: a second bind to a different store
/// drops the first.
///
/// `PropertyPanel` extends `View` so it can sit in a `PreferencesPanel`
/// page. It does *not* paint chrome in the headless / fallback path —
/// hosts can render the typed editors however they want by observing
/// `properties()` + per-property `on_change`. The widget is the
/// authoritative state holder.
///
/// License-lineage note: the typed variants mirror the conceptual menu
/// of the reference framework's `PropertyPanel`, but the surface
/// (`add_boolean`, `add_choice`, …, `bind_persistence`) is Pulp-native
/// and the persistence story leans on Pulp's existing
/// `state::PropertiesFile`.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <pulp/state/properties_file.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

class PropertyPanel : public View {
public:
    enum class Kind {
        BooleanToggle,  ///< bool — round-trips as 0/1
        Choice,         ///< single int index into `options`
        Slider,         ///< float in [min, max]
        Button,         ///< action only (no persisted state)
        MultiChoice,    ///< ordered set of int indices into `options`
        Text,           ///< std::string value
    };

    using Value = std::variant<bool,
                               int,
                               float,
                               std::vector<int>,
                               std::string,
                               std::monostate>;

    struct Property {
        std::string key;        ///< Persistence + lookup key.
        std::string label;      ///< Display label.
        Kind kind = Kind::Text;
        Value value{};
        // Slider-only constraints.
        float min = 0.0f;
        float max = 1.0f;
        // Choice / MultiChoice options.
        std::vector<std::string> options;
        // Per-property change observer fires AFTER the value updates +
        // BEFORE persistence is written.
        std::function<void(const Value&)> on_change;
        // Button-only action; fires on `click`.
        std::function<void()> on_click;
    };

    PropertyPanel() = default;

    // ── Builders (preferred entry points) ────────────────────────────

    void add_boolean(std::string key, std::string label, bool initial,
                     std::function<void(bool)> on_change = {}) {
        Property p;
        p.kind = Kind::BooleanToggle;
        p.key = std::move(key);
        p.label = std::move(label);
        p.value = initial;
        if (on_change) {
            p.on_change = [cb = std::move(on_change)](const Value& v) {
                if (auto* b = std::get_if<bool>(&v)) cb(*b);
            };
        }
        register_property(std::move(p));
    }

    void add_choice(std::string key, std::string label,
                    std::vector<std::string> options, int initial_index,
                    std::function<void(int)> on_change = {}) {
        Property p;
        p.kind = Kind::Choice;
        p.key = std::move(key);
        p.label = std::move(label);
        p.options = std::move(options);
        // Clamp the initial index defensively so the panel never holds
        // an out-of-range value.
        if (p.options.empty()) {
            p.value = 0;
        } else {
            if (initial_index < 0) initial_index = 0;
            if (initial_index >= static_cast<int>(p.options.size())) {
                initial_index = static_cast<int>(p.options.size()) - 1;
            }
            p.value = initial_index;
        }
        if (on_change) {
            p.on_change = [cb = std::move(on_change)](const Value& v) {
                if (auto* i = std::get_if<int>(&v)) cb(*i);
            };
        }
        register_property(std::move(p));
    }

    void add_slider(std::string key, std::string label, float min, float max,
                    float initial, std::function<void(float)> on_change = {}) {
        Property p;
        p.kind = Kind::Slider;
        p.key = std::move(key);
        p.label = std::move(label);
        p.min = min;
        p.max = max;
        p.value = clamp(initial, min, max);
        if (on_change) {
            p.on_change = [cb = std::move(on_change)](const Value& v) {
                if (auto* f = std::get_if<float>(&v)) cb(*f);
            };
        }
        register_property(std::move(p));
    }

    void add_button(std::string key, std::string label,
                    std::function<void()> on_click) {
        Property p;
        p.kind = Kind::Button;
        p.key = std::move(key);
        p.label = std::move(label);
        p.value = std::monostate{};
        p.on_click = std::move(on_click);
        register_property(std::move(p));
    }

    void add_multi_choice(std::string key, std::string label,
                          std::vector<std::string> options,
                          std::vector<int> initial_selection,
                          std::function<void(const std::vector<int>&)> on_change = {}) {
        Property p;
        p.kind = Kind::MultiChoice;
        p.key = std::move(key);
        p.label = std::move(label);
        p.options = std::move(options);
        normalize_indices(initial_selection, p.options.size());
        p.value = std::move(initial_selection);
        if (on_change) {
            p.on_change = [cb = std::move(on_change)](const Value& v) {
                if (auto* xs = std::get_if<std::vector<int>>(&v)) cb(*xs);
            };
        }
        register_property(std::move(p));
    }

    void add_text(std::string key, std::string label, std::string initial,
                  std::function<void(const std::string&)> on_change = {}) {
        Property p;
        p.kind = Kind::Text;
        p.key = std::move(key);
        p.label = std::move(label);
        p.value = std::move(initial);
        if (on_change) {
            p.on_change = [cb = std::move(on_change)](const Value& v) {
                if (auto* s = std::get_if<std::string>(&v)) cb(*s);
            };
        }
        register_property(std::move(p));
    }

    // ── Read access ──────────────────────────────────────────────────

    std::size_t size() const { return properties_.size(); }
    bool empty() const { return properties_.empty(); }

    const std::vector<Property>& properties() const { return properties_; }

    const Property* find(std::string_view key) const {
        for (const auto& p : properties_) {
            if (p.key == key) return &p;
        }
        return nullptr;
    }

    std::optional<bool> get_bool(std::string_view key) const {
        if (const auto* p = find(key))
            if (const auto* b = std::get_if<bool>(&p->value)) return *b;
        return std::nullopt;
    }
    std::optional<int> get_choice(std::string_view key) const {
        if (const auto* p = find(key))
            if (const auto* i = std::get_if<int>(&p->value)) return *i;
        return std::nullopt;
    }
    std::optional<float> get_slider(std::string_view key) const {
        if (const auto* p = find(key))
            if (const auto* f = std::get_if<float>(&p->value)) return *f;
        return std::nullopt;
    }
    std::optional<std::vector<int>> get_multi_choice(std::string_view key) const {
        if (const auto* p = find(key))
            if (const auto* xs = std::get_if<std::vector<int>>(&p->value)) return *xs;
        return std::nullopt;
    }
    std::optional<std::string> get_text(std::string_view key) const {
        if (const auto* p = find(key))
            if (const auto* s = std::get_if<std::string>(&p->value)) return *s;
        return std::nullopt;
    }

    // ── Mutation (drives observers + persistence) ────────────────────

    /// Set a Boolean property. No-op when the key is missing or the
    /// kind is not Boolean.
    bool set_bool(std::string_view key, bool v) {
        auto* p = find_mut(key);
        if (!p || p->kind != Kind::BooleanToggle) return false;
        p->value = v;
        fire(*p);
        return true;
    }
    bool set_choice(std::string_view key, int index) {
        auto* p = find_mut(key);
        if (!p || p->kind != Kind::Choice) return false;
        if (p->options.empty()) {
            p->value = 0;
        } else {
            if (index < 0) index = 0;
            if (index >= static_cast<int>(p->options.size())) {
                index = static_cast<int>(p->options.size()) - 1;
            }
            p->value = index;
        }
        fire(*p);
        return true;
    }
    bool set_slider(std::string_view key, float v) {
        auto* p = find_mut(key);
        if (!p || p->kind != Kind::Slider) return false;
        p->value = clamp(v, p->min, p->max);
        fire(*p);
        return true;
    }
    bool set_multi_choice(std::string_view key, std::vector<int> selection) {
        auto* p = find_mut(key);
        if (!p || p->kind != Kind::MultiChoice) return false;
        normalize_indices(selection, p->options.size());
        p->value = std::move(selection);
        fire(*p);
        return true;
    }
    bool set_text(std::string_view key, std::string v) {
        auto* p = find_mut(key);
        if (!p || p->kind != Kind::Text) return false;
        p->value = std::move(v);
        fire(*p);
        return true;
    }

    /// Click a Button property. No-op when the key is missing or the
    /// kind is not Button.
    bool click(std::string_view key) {
        auto* p = find_mut(key);
        if (!p || p->kind != Kind::Button) return false;
        if (p->on_click) p->on_click();
        return true;
    }

    // ── Simulation aliases (named for headless tests) ───────────────

    bool simulate_toggle(std::string_view key) {
        auto cur = get_bool(key);
        if (!cur) return false;
        return set_bool(key, !*cur);
    }
    bool simulate_choose(std::string_view key, int index) {
        return set_choice(key, index);
    }
    bool simulate_slide(std::string_view key, float v) {
        return set_slider(key, v);
    }
    bool simulate_click(std::string_view key) { return click(key); }
    bool simulate_multi_check(std::string_view key, int index, bool on) {
        auto cur = get_multi_choice(key);
        if (!cur) return false;
        std::vector<int> next = *cur;
        if (on) {
            // Insert sorted-unique.
            if (std::find(next.begin(), next.end(), index) == next.end()) {
                next.push_back(index);
            }
        } else {
            next.erase(std::remove(next.begin(), next.end(), index), next.end());
        }
        return set_multi_choice(key, std::move(next));
    }
    bool simulate_set_text(std::string_view key, std::string v) {
        return set_text(key, std::move(v));
    }

    // ── Persistence ──────────────────────────────────────────────────

    /// Bind the panel to a `PropertiesFile`. One-shot read populates
    /// every registered property whose key is present; subsequent
    /// mutations write back. Pass `nullptr` to detach.
    void bind_persistence(pulp::state::PropertiesFile* props) {
        props_ = props;
        if (!props_) return;
        for (auto& p : properties_) {
            load_from_props(p);
        }
    }
    bool has_persistence() const { return props_ != nullptr; }

private:
    void register_property(Property p) {
        // Deduplicate by key: re-registering the same key replaces the
        // old slot in place (callers might rebuild a panel on theme
        // switch, etc.).
        for (auto& existing : properties_) {
            if (existing.key == p.key) {
                existing = std::move(p);
                if (props_) load_from_props(existing);
                return;
            }
        }
        properties_.push_back(std::move(p));
        if (props_) load_from_props(properties_.back());
    }

    Property* find_mut(std::string_view key) {
        for (auto& p : properties_) {
            if (p.key == key) return &p;
        }
        return nullptr;
    }

    void fire(const Property& p) {
        if (p.on_change) p.on_change(p.value);
        persist(p);
    }

    void persist(const Property& p) const {
        if (!props_) return;
        switch (p.kind) {
            case Kind::BooleanToggle:
                if (auto* b = std::get_if<bool>(&p.value))
                    props_->set_bool(p.key, *b);
                break;
            case Kind::Choice:
                if (auto* i = std::get_if<int>(&p.value))
                    props_->set_int(p.key, *i);
                break;
            case Kind::Slider:
                if (auto* f = std::get_if<float>(&p.value))
                    props_->set_double(p.key, static_cast<double>(*f));
                break;
            case Kind::MultiChoice:
                if (auto* xs = std::get_if<std::vector<int>>(&p.value))
                    props_->set_string(p.key, encode_indices(*xs));
                break;
            case Kind::Text:
                if (auto* s = std::get_if<std::string>(&p.value))
                    props_->set_string(p.key, *s);
                break;
            case Kind::Button:
                break;  // No persisted state.
        }
    }

    void load_from_props(Property& p) {
        if (!props_) return;
        switch (p.kind) {
            case Kind::BooleanToggle:
                if (auto v = props_->get_bool(p.key)) p.value = *v;
                break;
            case Kind::Choice:
                if (auto v = props_->get_int(p.key)) {
                    int i = static_cast<int>(*v);
                    if (!p.options.empty()) {
                        if (i < 0) i = 0;
                        if (i >= static_cast<int>(p.options.size())) {
                            i = static_cast<int>(p.options.size()) - 1;
                        }
                    } else {
                        i = 0;
                    }
                    p.value = i;
                }
                break;
            case Kind::Slider:
                if (auto v = props_->get_double(p.key)) {
                    p.value = clamp(static_cast<float>(*v), p.min, p.max);
                }
                break;
            case Kind::MultiChoice:
                if (auto v = props_->get_string(p.key)) {
                    auto xs = decode_indices(*v);
                    normalize_indices(xs, p.options.size());
                    p.value = std::move(xs);
                }
                break;
            case Kind::Text:
                if (auto v = props_->get_string(p.key)) p.value = *v;
                break;
            case Kind::Button:
                break;
        }
    }

    static float clamp(float v, float lo, float hi) {
        if (hi < lo) return v;
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }
    static void normalize_indices(std::vector<int>& xs, std::size_t option_count) {
        std::unordered_set<int> seen;
        std::vector<int> out;
        out.reserve(xs.size());
        for (int i : xs) {
            if (option_count > 0
                && (i < 0 || i >= static_cast<int>(option_count))) continue;
            if (seen.insert(i).second) out.push_back(i);
        }
        std::sort(out.begin(), out.end());
        xs.swap(out);
    }
    static std::string encode_indices(const std::vector<int>& xs) {
        std::string out;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            if (i) out.push_back(',');
            out += std::to_string(xs[i]);
        }
        return out;
    }
    static std::vector<int> decode_indices(std::string_view s) {
        std::vector<int> out;
        std::size_t i = 0;
        while (i < s.size()) {
            std::size_t j = s.find(',', i);
            if (j == std::string_view::npos) j = s.size();
            auto tok = s.substr(i, j - i);
            // Trim whitespace.
            while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
                tok.remove_prefix(1);
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
                tok.remove_suffix(1);
            if (!tok.empty()) {
                try {
                    out.push_back(std::stoi(std::string(tok)));
                } catch (...) {
                    // Skip malformed entries — defensive against
                    // hand-edited properties files.
                }
            }
            i = j + 1;
        }
        return out;
    }

    std::vector<Property> properties_;
    pulp::state::PropertiesFile* props_ = nullptr;
};

} // namespace pulp::view
