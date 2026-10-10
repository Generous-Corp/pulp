#include <pulp/view/design_import.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/screenshot.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

#include "text_diagnostic_observer.hpp"
#include <pulp/view/widgets.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

bool parse_positive(const char* value, float& result) {
    try {
        result = std::stof(value);
        return result > 0.0f;
    } catch (...) {
        return false;
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text << '\n';
    return static_cast<bool>(output);
}

void usage() {
    std::cerr << "Usage: pulp-design-ir-observe --input <design.ir.json> "
                 "--render <png> --layout <json> --width <px> --height <px> "
                 "[--scale <factor>] [--text-diagnostics <json>] [--set-value "
                 "<anchor>=<normalized>]... [--click <x>,<y>]... "
                 "[--drag <x1>,<y1>:<x2>,<y2>]... [--key <name>]... "
                 "[--interaction-log <json>]\n";
}

// One replayed input step. Steps run in command-line order after layout and
// before the render, so the PNG and layout reflect the post-interaction state.
struct InteractionStep {
    enum class Kind { click, drag, key };
    Kind kind = Kind::click;
    pulp::view::Point start{};
    pulp::view::Point end{};
    std::string key_name;
    pulp::view::KeyCode key = pulp::view::KeyCode::unknown;
    std::uint16_t modifiers = 0;
};

std::optional<pulp::view::Point> parse_point(std::string_view text) {
    const auto comma = text.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    try {
        const std::string xs{text.substr(0, comma)};
        const std::string ys{text.substr(comma + 1)};
        std::size_t xn = 0;
        std::size_t yn = 0;
        const float x = std::stof(xs, &xn);
        const float y = std::stof(ys, &yn);
        if (xn != xs.size() || yn != ys.size()) return std::nullopt;
        return pulp::view::Point{x, y};
    } catch (...) {
        return std::nullopt;
    }
}

// Key names accepted by --key: an optional "shift+" prefix, then a named key
// or a single lowercase letter or digit.
std::optional<std::pair<pulp::view::KeyCode, std::uint16_t>> parse_key(
    std::string_view text) {
    using pulp::view::KeyCode;
    std::uint16_t modifiers = 0;
    constexpr std::string_view shift_prefix = "shift+";
    if (text.substr(0, shift_prefix.size()) == shift_prefix) {
        modifiers = pulp::view::kModShift;
        text.remove_prefix(shift_prefix.size());
    }
    static const std::pair<std::string_view, KeyCode> named[] = {
        {"tab", KeyCode::tab},       {"enter", KeyCode::enter},
        {"escape", KeyCode::escape}, {"space", KeyCode::space},
        {"left", KeyCode::left},     {"right", KeyCode::right},
        {"up", KeyCode::up},         {"down", KeyCode::down},
        {"home", KeyCode::home},     {"end", KeyCode::end_},
        {"page_up", KeyCode::page_up}, {"page_down", KeyCode::page_down},
        {"backspace", KeyCode::backspace}, {"delete", KeyCode::delete_},
    };
    for (const auto& [name, code] : named)
        if (text == name) return std::pair{code, modifiers};
    if (text.size() == 1 && ((text[0] >= 'a' && text[0] <= 'z') ||
                             (text[0] >= '0' && text[0] <= '9')))
        return std::pair{static_cast<KeyCode>(text[0]), modifiers};
    return std::nullopt;
}

// The nearest DesignIR anchor at or above a view; hit-test results land on
// unanchored internals (a knob's label, a panel's background) as often as on
// the imported node itself.
std::string anchor_of(const pulp::view::View* view) {
    for (; view; view = view->parent())
        if (!view->anchor_id().empty()) return view->anchor_id();
    return {};
}

// Child-index path from the root ("" for the root, "0/2" for the third child
// of the first child). Disambiguates views that share an anchor.
std::string path_of(const pulp::view::View* view) {
    std::string path;
    for (; view && view->parent(); view = view->parent()) {
        const auto* parent = view->parent();
        for (std::size_t i = 0; i < parent->child_count(); ++i) {
            if (parent->child_at(i) != view) continue;
            path = path.empty() ? std::to_string(i)
                                : std::to_string(i) + "/" + path;
            break;
        }
    }
    return path;
}

pulp::view::View* find_focused(pulp::view::View& root) {
    if (root.has_focus()) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* found = find_focused(*root.child_at(i))) return found;
    return nullptr;
}

// Observable control state in tree order: the values a pointer or key event is
// expected to change. An array, not an anchor-keyed map, because a DesignIR
// anchor may legitimately label more than one materialized view.
void collect_state(const pulp::view::View& view, choc::value::Value& out) {
    if (!view.anchor_id().empty()) {
        auto entry = choc::value::createObject("");
        entry.addMember("anchor", choc::value::createString(view.anchor_id()));
        entry.addMember("path", choc::value::createString(path_of(&view)));
        bool stateful = true;
        if (auto* knob = dynamic_cast<const pulp::view::Knob*>(&view)) {
            entry.addMember("kind", choc::value::createString("knob"));
            entry.addMember("value", choc::value::createFloat64(knob->value()));
        } else if (auto* fader = dynamic_cast<const pulp::view::Fader*>(&view)) {
            entry.addMember("kind", choc::value::createString("fader"));
            entry.addMember("value", choc::value::createFloat64(fader->value()));
        } else if (auto* slider =
                       dynamic_cast<const pulp::view::RangeSlider*>(&view)) {
            entry.addMember("kind", choc::value::createString("range_slider"));
            entry.addMember("value", choc::value::createFloat64(slider->value()));
        } else if (auto* toggle =
                       dynamic_cast<const pulp::view::Toggle*>(&view)) {
            entry.addMember("kind", choc::value::createString("toggle"));
            entry.addMember("on", choc::value::createBool(toggle->is_on()));
        } else if (auto* button =
                       dynamic_cast<const pulp::view::ToggleButton*>(&view)) {
            entry.addMember("kind", choc::value::createString("toggle_button"));
            entry.addMember("on", choc::value::createBool(button->is_on()));
        } else if (auto* box =
                       dynamic_cast<const pulp::view::Checkbox*>(&view)) {
            entry.addMember("kind", choc::value::createString("checkbox"));
            entry.addMember("checked", choc::value::createBool(box->is_checked()));
        } else {
            stateful = false;
        }
        if (stateful) out.addArrayElement(entry);
    }
    for (std::size_t i = 0; i < view.child_count(); ++i)
        collect_state(*view.child_at(i), out);
}

choc::value::Value replay_step(pulp::view::View& root,
                               const InteractionStep& step,
                               std::size_t index) {
    using Kind = InteractionStep::Kind;
    auto record = choc::value::createObject("");
    record.addMember("index", choc::value::createInt64(
                                  static_cast<std::int64_t>(index)));
    bool handled = false;
    const pulp::view::View* target_view = nullptr;
    if (step.kind == Kind::click || step.kind == Kind::drag) {
        record.addMember("kind", choc::value::createString(
                                     step.kind == Kind::click ? "click" : "drag"));
        record.addMember("x", choc::value::createFloat64(step.start.x));
        record.addMember("y", choc::value::createFloat64(step.start.y));
        target_view = root.hit_test(step.start);
        if (step.kind == Kind::click) {
            root.simulate_click(step.start);
        } else {
            record.addMember("to_x", choc::value::createFloat64(step.end.x));
            record.addMember("to_y", choc::value::createFloat64(step.end.y));
            root.simulate_drag(step.start, step.end);
        }
        handled = target_view != nullptr && target_view != &root;
    } else {
        record.addMember("kind", choc::value::createString("key"));
        record.addMember("key", choc::value::createString(step.key_name));
        auto* focused = find_focused(root);
        if (step.key == pulp::view::KeyCode::tab) {
            // Focus traversal belongs to the host; replay it the way the
            // window hosts do, so Tab is observable without a window.
            auto* next = (step.modifiers & pulp::view::kModShift)
                             ? pulp::view::View::focus_prev(root, focused)
                             : pulp::view::View::focus_next(root, focused);
            if (next) next->on_focus_changed(true);
            if (focused && focused != next) focused->on_focus_changed(false);
            handled = next != nullptr;
            target_view = next;
        } else if (focused) {
            pulp::view::KeyEvent down;
            down.key = step.key;
            down.modifiers = step.modifiers;
            handled = focused->on_key_event(down);
            pulp::view::KeyEvent up = down;
            up.is_down = false;
            focused->on_key_event(up);
            target_view = focused;
        }
    }
    record.addMember("target", choc::value::createString(anchor_of(target_view)));
    record.addMember("target_path",
                     choc::value::createString(target_view ? path_of(target_view)
                                                           : std::string{"-"}));
    record.addMember("handled", choc::value::createBool(handled));
    const auto* focused_after = find_focused(root);
    record.addMember("focused", choc::value::createString(anchor_of(focused_after)));
    record.addMember("focused_path",
                     choc::value::createString(focused_after ? path_of(focused_after)
                                                             : std::string{"-"}));
    auto state = choc::value::createEmptyArray();
    collect_state(root, state);
    record.addMember("state", state);
    return record;
}

std::optional<std::pair<std::string, float>> parse_value_override(
    std::string_view text) {
    const auto separator = text.rfind('=');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == text.size())
        return std::nullopt;
    try {
        std::size_t consumed = 0;
        const std::string value_text{text.substr(separator + 1)};
        const float value = std::stof(value_text, &consumed);
        if (consumed != value_text.size() || value < 0.0f || value > 1.0f)
            return std::nullopt;
        return std::pair{std::string{text.substr(0, separator)}, value};
    } catch (...) {
        return std::nullopt;
    }
}

pulp::view::View* find_by_anchor(pulp::view::View& root,
                                 std::string_view anchor) {
    if (root.anchor_id() == anchor) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* found = find_by_anchor(*root.child_at(i), anchor))
            return found;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path input_path;
    std::filesystem::path render_path;
    std::filesystem::path layout_path;
    std::filesystem::path text_diagnostics_path;
    float width = 0.0f;
    float height = 0.0f;
    float scale = 2.0f;
    std::vector<std::pair<std::string, float>> value_overrides;
    std::vector<InteractionStep> steps;
    std::filesystem::path interaction_log_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        const char* value = argv[++i];
        if (arg == "--input") input_path = value;
        else if (arg == "--render") render_path = value;
        else if (arg == "--layout") layout_path = value;
        else if (arg == "--text-diagnostics")
            text_diagnostics_path = value;
        else if (arg == "--width" && parse_positive(value, width)) {}
        else if (arg == "--height" && parse_positive(value, height)) {}
        else if (arg == "--scale" && parse_positive(value, scale)) {}
        else if (arg == "--set-value") {
            auto parsed = parse_value_override(value);
            if (!parsed) {
                std::cerr << "Error: --set-value expects "
                             "<anchor>=<normalized 0..1>\n";
                return 2;
            }
            value_overrides.push_back(std::move(*parsed));
        }
        else if (arg == "--click") {
            auto point = parse_point(value);
            if (!point) {
                std::cerr << "Error: --click expects <x>,<y>\n";
                return 2;
            }
            steps.push_back({.kind = InteractionStep::Kind::click,
                             .start = *point});
        }
        else if (arg == "--drag") {
            const std::string_view text{value};
            const auto colon = text.find(':');
            auto from = colon == std::string_view::npos
                            ? std::nullopt
                            : parse_point(text.substr(0, colon));
            auto to = colon == std::string_view::npos
                          ? std::nullopt
                          : parse_point(text.substr(colon + 1));
            if (!from || !to) {
                std::cerr << "Error: --drag expects <x1>,<y1>:<x2>,<y2>\n";
                return 2;
            }
            steps.push_back({.kind = InteractionStep::Kind::drag,
                             .start = *from,
                             .end = *to});
        }
        else if (arg == "--key") {
            auto key = parse_key(value);
            if (!key) {
                std::cerr << "Error: --key does not name a supported key: "
                          << value << "\n";
                return 2;
            }
            steps.push_back({.kind = InteractionStep::Kind::key,
                             .key_name = value,
                             .key = key->first,
                             .modifiers = key->second});
        }
        else if (arg == "--interaction-log") interaction_log_path = value;
        else {
            usage();
            return 2;
        }
    }
    if (input_path.empty() || render_path.empty() || layout_path.empty() ||
        width <= 0.0f || height <= 0.0f) {
        usage();
        return 2;
    }
    if (!steps.empty() && interaction_log_path.empty()) {
        // Replayed input whose outcome is not recorded cannot be compared;
        // refuse it rather than rendering a state nobody can audit.
        std::cerr << "Error: --click/--drag/--key require --interaction-log\n";
        return 2;
    }
    const auto serialized = read_text(input_path);
    if (serialized.empty()) {
        std::cerr << "Error: could not read DesignIR input\n";
        return 1;
    }
    auto ir = pulp::view::parse_design_ir_json(serialized);
    // The document's own directory is the search root for its manifest assets:
    // relative local_paths resolve against it, and a local_path that no longer
    // points at its bytes is recovered by content hash from the asset folders
    // beside it. Without this the tool depends on the process CWD.
    auto root = pulp::view::build_native_view_tree(
        ir, ir.asset_manifest,
        {.asset_base_directory = input_path.parent_path()});
    if (!root) {
        std::cerr << "Error: could not materialize DesignIR\n";
        return 1;
    }
    for (const auto& [anchor, value] : value_overrides) {
        auto* view = find_by_anchor(*root, anchor);
        if (!view) {
            std::cerr << "Error: no imported view has anchor '" << anchor
                      << "'\n";
            return 1;
        }
        if (auto* knob = dynamic_cast<pulp::view::Knob*>(view))
            knob->set_value(value);
        else if (auto* fader = dynamic_cast<pulp::view::Fader*>(view))
            fader->set_value(value);
        else {
            std::cerr << "Error: imported view '" << anchor
                      << "' is not a knob or fader\n";
            return 1;
        }
        std::cerr << "value override: " << anchor << '=' << value << "\n";
    }
    root->set_bounds({0.0f, 0.0f, width, height});
    if (!interaction_log_path.empty()) {
        root->layout_children();
        auto log = choc::value::createObject("");
        log.addMember("schema", choc::value::createString(
                                    "pulp-design-ir-interaction-log-v1"));
        log.addMember("fixture",
                      choc::value::createString(input_path.filename().string()));
        auto initial = choc::value::createEmptyArray();
        collect_state(*root, initial);
        log.addMember("initial_state", initial);
        auto records = choc::value::createEmptyArray();
        for (std::size_t i = 0; i < steps.size(); ++i)
            records.addArrayElement(replay_step(*root, steps[i], i));
        log.addMember("steps", records);
        if (!write_text(interaction_log_path, choc::json::toString(log, true))) {
            std::cerr << "Error: could not write interaction log\n";
            return 1;
        }
    }
    if (!text_diagnostics_path.empty())
        pulp::import_design::enable_text_observation(*root);
    // Which line-breaking path each Label took. Reported unconditionally
    // because a cache that never activates and one that always does produce
    // the same pixels when the reflow happens to agree — and only one of those
    // is the mechanism working.
    pulp::view::Label::reset_line_break_path_counts();
    if (!pulp::view::render_to_file(
            *root,
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            render_path.string(),
            scale,
            pulp::view::ScreenshotBackend::skia)) {
        std::cerr << "Error: could not render DesignIR through Skia\n";
        return 1;
    }
    const auto paths = pulp::view::Label::line_break_path_counts();
    std::cerr << "line-break paths: cached=" << paths.cached
              << " reflowed=" << paths.reflowed
              << " uncached=" << paths.uncached << "\n";

    const auto layout = pulp::view::dump_layout_tree(
        *root,
        {.surface = "design-ir-observer",
         .fixture = input_path.filename().string(),
         .viewport_width = width,
         .viewport_height = height});
    if (!write_text(layout_path, layout)) {
        std::cerr << "Error: could not write layout observation\n";
        return 1;
    }
    if (!text_diagnostics_path.empty() &&
        !write_text(text_diagnostics_path,
                    pulp::import_design::observe_text(*root, input_path.string()))) {
        std::cerr << "Error: could not write text observation\n";
        return 1;
    }
    return 0;
}
