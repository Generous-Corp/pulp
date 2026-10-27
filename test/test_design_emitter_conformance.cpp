// Emitter conformance gate.
//
// One DesignIR reaches a plugin through four emitters that each walk the IR on
// their own: the native materializer, the ui.js bridge program, the baked C++
// exporter and the SwiftUI exporter. A lowering added to one of them (a
// theme-painted control becoming a widget, a snapped paint rect) does not reach
// the others unless someone ports it, and nothing said so. This gate builds the
// same fixtures through every lane and compares an emitter-neutral projection
// of what each one produced:
//
//   - the tree of anchored views (parent anchor and order among anchored
//     siblings), which is the identity bindings and tweaks address;
//   - each anchored view's widget kind;
//   - the HTML state of a theme-painted range, checkbox or radio;
//   - the captured snapped paint rect, and the absolute self_paint_bounds()
//     after layout for every view that has one;
//   - the bindings each lane installs, keyed by anchor.
//
// The native materializer is the reference. Every divergence must match an
// entry in test/fixtures/emitter-conformance/allowlist.json, and every entry
// must link an issue and still match something, so a gap is either fixed or
// written down and a fixed gap cannot leave a stale excuse behind.
//
// SwiftUI is generated as source, so only what its binding manifest declares
// (which anchors bind to what) is compared for that lane.

#include <pulp/state/store.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/data_display.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/design_sources.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

#include "support/unique_temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

#if defined(__GNUG__)
#include <cstdlib>
#include <cxxabi.h>
#endif

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT ""
#endif

using namespace pulp::view;
namespace fs = std::filesystem;

// The baked C++ of each fixture, generated into the build tree by
// pulp-emitter-conformance-gen (test/cmake/design_import_native_codegen_tests.cmake).
#define PULP_CONFORMANCE_FIXTURE(ns)                                                               \
    namespace pulp::test::conformance::ns {                                                        \
    std::unique_ptr<pulp::view::View> conformance_build(const std::filesystem::path& assets);      \
    void conformance_bind(pulp::view::View& root, pulp::view::NativeImportBindingContext& ctx);    \
    }
PULP_CONFORMANCE_FIXTURE(form_controls_08)
PULP_CONFORMANCE_FIXTURE(spectr)
PULP_CONFORMANCE_FIXTURE(kelvin)
PULP_CONFORMANCE_FIXTURE(keyed_list_18)
PULP_CONFORMANCE_FIXTURE(fidelity_scene)
#undef PULP_CONFORMANCE_FIXTURE

namespace {

// ── projection ──────────────────────────────────────────────────────────────

/// anchor -> field -> value. Fields: present, parent, order, kind, state.*,
/// paint.captured, paint.bounds, binding.
using Projection = std::map<std::string, std::map<std::string, std::string>>;

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string kind_of(const View& view) {
    std::string name = typeid(view).name();
#if defined(__GNUG__)
    int status = 0;
    if (char* demangled = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status)) {
        if (status == 0)
            name = demangled;
        std::free(demangled);
    }
#endif
    for (const char* prefix : {"class ", "struct "})
        if (name.rfind(prefix, 0) == 0)
            name.erase(0, std::char_traits<char>::length(prefix));
    return name;
}

std::string number(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

std::string skin_name(UaSkin skin) {
    return skin == UaSkin::chromium_refresh ? "chromium_refresh" : "platform";
}

std::string scheme_name(UaColorScheme scheme) {
    return scheme == UaColorScheme::dark ? "dark" : "light";
}

std::string colour(pulp::canvas::Color c) {
    const auto byte = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b),
                  byte(c.a));
    return buffer;
}

/// The anchored view itself, or its nearest anchored ancestor.
const View* anchored(const View* view) {
    while (view && view->anchor_id().empty())
        view = view->parent();
    return view;
}

void project_state(const View& view, std::map<std::string, std::string>& fields) {
    if (const auto* range = dynamic_cast<const RangeSlider*>(&view)) {
        fields["state.min"] = number(range->min_value());
        fields["state.max"] = number(range->max_value());
        fields["state.step"] = number(range->step());
        fields["state.value"] = number(range->value());
        fields["state.skin"] = skin_name(range->skin());
        fields["state.scheme"] = scheme_name(range->color_scheme());
        fields["state.enabled"] = range->enabled() ? "true" : "false";
        if (range->has_accent_color())
            fields["state.accent"] = colour(range->accent_color());
    } else if (const auto* box = dynamic_cast<const Checkbox*>(&view)) {
        fields["state.control"] = box->kind() == Checkbox::Kind::radio ? "radio" : "checkbox";
        fields["state.checked"] = box->is_checked() ? "true" : "false";
        fields["state.skin"] = skin_name(box->skin());
        fields["state.scheme"] = scheme_name(box->color_scheme());
        fields["state.enabled"] = box->enabled() ? "true" : "false";
        if (box->kind() == Checkbox::Kind::radio)
            fields["state.radio_group"] = std::to_string(box->radio_group());
        if (const auto accent = box->accent_color())
            fields["state.accent"] = colour(*accent);
    } else if (const auto* surface = dynamic_cast<const ButtonSurface*>(&view)) {
        fields["state.control"] = "button";
        fields["state.scheme"] = scheme_name(surface->color_scheme());
        fields["state.enabled"] = surface->enabled() ? "true" : "false";
    }
}

void project_view(const View& view, const std::string& parent, float origin_x, float origin_y,
                  std::map<std::string, int>& order, Projection& out) {
    const float x = origin_x + view.bounds().x;
    const float y = origin_y + view.bounds().y;
    std::string next_parent = parent;
    if (const auto& anchor = view.anchor_id(); !anchor.empty()) {
        auto& fields = out[anchor];
        if (!fields.empty()) {
            // Two views carry one anchor; the conformance of the second is the
            // repeat itself.
            fields["repeats"] = std::to_string(
                std::stoi(fields["repeats"].empty() ? std::string("0") : fields["repeats"]) + 1);
        } else {
            fields["present"] = "yes";
            fields["parent"] = parent;
            fields["order"] = std::to_string(order[parent]++);
            fields["kind"] = kind_of(view);
            project_state(view, fields);
            if (view.has_captured_box_paint_rect()) {
                fields["paint.captured"] = number(view.captured_box_paint_left()) + "," +
                                           number(view.captured_box_paint_top()) + "," +
                                           number(view.captured_box_paint_right()) + "," +
                                           number(view.captured_box_paint_bottom());
                const auto paint = view.self_paint_bounds();
                fields["paint.bounds"] = number(x + paint.x) + "," + number(y + paint.y) + "," +
                                         number(paint.width) + "," + number(paint.height);
            }
        }
        next_parent = anchor;
    }
    for (std::size_t i = 0; i < view.child_count(); ++i)
        project_view(*view.child_at(i), next_parent, x, y, order, out);
}

Projection project_tree(const View& root) {
    Projection out;
    std::map<std::string, int> order;
    project_view(root, "", -root.bounds().x, -root.bounds().y, order, out);
    return out;
}

/// The key a meter reads: its value key when the design names one, otherwise
/// `source.channel`, which is what ui.js binds. Projecting the source alone
/// would drop the channel and call two lanes that read the same meter apart.
std::string meter_key(std::string_view value_key, std::string_view source,
                      std::string_view channel) {
    if (!value_key.empty())
        return std::string(value_key);
    if (channel.empty())
        return std::string(source);
    return std::string(source) + "." + std::string(channel);
}

void add_binding(Projection& out, const View* view, const std::string& binding) {
    const View* owner = anchored(view);
    auto& value = out[owner ? owner->anchor_id() : std::string("<unanchored>")]["binding"];
    value += (value.empty() ? "" : " ") + binding;
}

std::string key(std::string_view text) {
    return std::string(text);
}

/// Records what the native and baked binders bind, by anchor.
class RecordingBindingContext final : public NativeImportBindingContext {
  public:
    explicit RecordingBindingContext(Projection& out) : out_(out) {}

    void bind_knob(Knob& v, const NativeImportBindingDescriptor& d) override {
        add_binding(out_, &v, "param:" + key(d.param_key));
    }
    void bind_fader(Fader& v, const NativeImportBindingDescriptor& d) override {
        add_binding(out_, &v, "param:" + key(d.param_key));
    }
    void bind_toggle_button(ToggleButton& v, const NativeImportBindingDescriptor& d) override {
        add_binding(out_, &v, "param:" + key(d.param_key));
    }
    void bind_checkbox(Checkbox& v, const NativeImportBindingDescriptor& d) override {
        add_binding(out_, &v, "param:" + key(d.param_key));
    }
    void bind_choice_button(ToggleButton& v,
                            const NativeImportChoiceBindingDescriptor& d) override {
        add_binding(out_, &v, "choice:" + key(d.param_key) + "=" + key(d.choice_value));
    }
    void bind_xy_pad(XYPad& v, const NativeImportXYPadBindingDescriptor& d) override {
        add_binding(out_, &v, "xy:" + key(d.x_param_key) + "," + key(d.y_param_key));
    }
    void bind_text_editor(TextEditor& v, const NativeImportTextBindingDescriptor& d) override {
        add_binding(out_, &v, "text:" + key(d.value_key));
    }
    void bind_host_action(TextButton& v, const NativeImportHostActionDescriptor& d) override {
        add_binding(out_, &v, "action:" + key(d.action));
    }
    void bind_segmented(SegmentedControl& v,
                        const NativeImportSegmentedBindingDescriptor& d) override {
        add_binding(out_, &v, "param:" + key(d.param_key));
    }
    void bind_stepper(Stepper& v, const NativeImportStepperBindingDescriptor& d) override {
        add_binding(out_, &v, "param:" + key(d.param_key));
    }
    void bind_level_display(View& v, LevelDisplay&,
                            const NativeImportMeterBindingDescriptor& d) override {
        add_binding(out_, &v, "meter:" + meter_key(d.value_key, d.meter_source, d.channel));
    }
    void bind_series_display(View& v, SeriesDisplay&,
                             const NativeImportWaveformBindingDescriptor& d) override {
        add_binding(out_, &v, "series:" + key(d.param_key));
    }

  private:
    Projection& out_;
};

// ── lanes ───────────────────────────────────────────────────────────────────

struct Fixture {
    std::string name;
    fs::path file;
    bool figma_plugin = false;
    std::function<std::unique_ptr<View>(const fs::path&)> build_cpp;
    std::function<void(View&, NativeImportBindingContext&)> bind_cpp;
};

std::vector<Fixture> fixtures() {
    namespace c = pulp::test::conformance;
    const fs::path root(PULP_REPO_ROOT);
    const fs::path dir = root / "test/fixtures/emitter-conformance";
    return {
        {"form-controls-08", dir / "form-controls-08.ir.json", false,
         c::form_controls_08::conformance_build, c::form_controls_08::conformance_bind},
        {"spectr", dir / "spectr.ir.json", false, c::spectr::conformance_build,
         c::spectr::conformance_bind},
        {"kelvin", dir / "kelvin.ir.json", false, c::kelvin::conformance_build,
         c::kelvin::conformance_bind},
        {"keyed-list-18", dir / "keyed-list-18.ir.json", false, c::keyed_list_18::conformance_build,
         c::keyed_list_18::conformance_bind},
        {"fidelity-scene", root / "test/fixtures/import-fidelity/scene.pulp.json", true,
         c::fidelity_scene::conformance_build, c::fidelity_scene::conformance_bind},
    };
}

DesignIR load(const Fixture& fixture) {
    const auto json = read_file(fixture.file);
    return fixture.figma_plugin ? parse_figma_plugin_json(json) : parse_design_ir_json(json);
}

/// A valid 1x1 PNG, CRCs included, so every lane resolves and decodes an image.
constexpr unsigned char kTinyPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
    0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
    0x9c, 0x63, 0x68, 0x68, 0x68, 0xf8, 0x0f, 0x00, 0x05, 0x84, 0x02, 0x80, 0x8c, 0xcd,
    0x66, 0x26, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

/// The fixtures do not commit their captured bitmaps: the gate compares
/// structure, not pixels. Every image a fixture references is staged as a tiny
/// valid PNG instead, so an image node lowers as an image in every lane rather
/// than as whatever each lane does with a missing file.
class StagedAssets {
  public:
    explicit StagedAssets(const DesignIR& ir)
        : dir_(pulp::test::make_unique_temp_dir("pulp-emitter-conformance")) {
        std::set<std::string> paths;
        const auto visit = [&](auto& self, const IRNode& node) -> void {
            if (const auto it = node.attributes.find("asset_path"); it != node.attributes.end())
                paths.insert(it->second);
            for (const auto& child : node.children)
                self(self, child);
        };
        visit(visit, ir.root);
        for (const auto& asset : ir.asset_manifest.assets)
            if (asset.local_path)
                paths.insert(*asset.local_path);
        for (const auto& relative : paths) {
            const fs::path path(relative);
            if (path.empty() || path.is_absolute())
                continue;
            fs::create_directories((dir_ / path).parent_path());
            std::ofstream(dir_ / path, std::ios::binary)
                .write(reinterpret_cast<const char*>(kTinyPng), sizeof(kTinyPng));
        }
    }
    ~StagedAssets() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& dir() const {
        return dir_;
    }

  private:
    fs::path dir_;
};

Rect root_rect(const DesignIR& ir) {
    return {0.0f, 0.0f, ir.root.style.width.value_or(800.0f),
            ir.root.style.height.value_or(600.0f)};
}

Projection native_lane(const DesignIR& ir, const fs::path& assets) {
    auto root = build_native_view_tree(ir, ir.asset_manifest,
                                       {.preview_mode = true, .asset_base_directory = assets});
    REQUIRE(root != nullptr);
    root->set_bounds(root_rect(ir));
    root->layout_children();
    auto out = project_tree(*root);
    RecordingBindingContext binding(out);
    bind_native_view_tree(*root, ir, binding);
    return out;
}

Projection cpp_lane(const Fixture& fixture, const DesignIR& ir, const fs::path& assets) {
    auto root = fixture.build_cpp(assets);
    REQUIRE(root != nullptr);
    root->set_bounds(root_rect(ir));
    root->layout_children();
    auto out = project_tree(*root);
    RecordingBindingContext binding(out);
    fixture.bind_cpp(*root, binding);
    return out;
}

/// Records the bridge's binding calls before forwarding them, so the JS lane's
/// bindings are observed exactly as the program issued them. A binding the
/// bridge rejects (an undeclared parameter) must not stop the program.
constexpr const char* kBindingProbe = R"js(
var __pulpConformance = {params: [], meters: []};
(function () {
    var bindParam = bindWidgetToParam;
    bindWidgetToParam = function (id, paramKey) {
        __pulpConformance.params.push([String(id), String(paramKey)]);
        try { return bindParam.apply(this, arguments); } catch (e) { return undefined; }
    };
    if (typeof bindMeter === 'function') {
        var bindMeterNative = bindMeter;
        bindMeter = function (id, source) {
            __pulpConformance.meters.push([String(id), String(source)]);
            try { return bindMeterNative.apply(this, arguments); } catch (e) { return undefined; }
        };
    }
})();
)js";

Projection js_lane(const DesignIR& ir, const fs::path& assets) {
    View host;
    host.set_bounds(root_rect(ir));
    ScriptEngine engine;
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, host, store);
    bridge.set_script_base_dir(assets);
    bridge.load_script(std::string(kBindingProbe) + generate_pulp_js(ir, CodeGenOptions{}));
    host.layout_children();
    REQUIRE(host.child_count() > 0);
    auto out = project_tree(*host.child_at(0));

    const auto recorded =
        choc::json::parse(engine.evaluate("JSON.stringify(__pulpConformance)").toString());
    for (const char* list : {"params", "meters"}) {
        const auto rows = recorded[list];
        for (uint32_t i = 0; i < rows.size(); ++i) {
            const std::string id(rows[i][0].toString());
            const std::string target(rows[i][1].toString());
            add_binding(out, bridge.widget(id),
                        std::string(list[0] == 'p' ? "param:" : "meter:") + target);
        }
    }
    return out;
}

Projection swift_lane(const DesignIR& ir) {
    const auto generated = generate_pulp_swift(ir, ir.asset_manifest, {});
    Projection out;
    if (generated.binding_manifest.empty())
        return out;
    const auto manifest = choc::json::parse(generated.binding_manifest);
    const auto entries = manifest["entries"];
    for (uint32_t i = 0; i < entries.size(); ++i) {
        const auto e = entries[i];
        // The manifest also lists a control's binding contract when SwiftUI
        // does not bind it; only an entry with a resolution block is bound.
        if (!e.hasObjectMember("resolve_name"))
            continue;
        const std::string anchor(e["anchor_id"].toString());
        const std::string family(e["source_family"].toString());
        // A meter projects the key it reads, as the native lane records it,
        // whether the manifest marks it a meter by source family or only by
        // its primitive (a key-only meter carries no family).
        const bool meter = family == "meter" || e["native_primitive"].toString() == "meter";
        std::string binding =
            meter ? "meter:" + meter_key(std::string(e["meter_value_key"].toString()),
                                         std::string(e["meter_source"].toString()),
                                         std::string(e["meter_channel"].toString()))
                  : "param:" + std::string(e["param_key"].toString());
        auto& value = out[anchor]["binding"];
        value += (value.empty() ? "" : " ") + binding;
    }
    return out;
}

// ── comparison ──────────────────────────────────────────────────────────────

struct Divergence {
    std::string lane, fixture, anchor, field, expected, actual;
};

bool bounds_close(const std::string& a, const std::string& b) {
    std::vector<double> x, y;
    for (auto* pair : {&a, &b}) {
        auto& values = pair == &a ? x : y;
        std::stringstream in(*pair);
        std::string part;
        while (std::getline(in, part, ','))
            values.push_back(std::stod(part));
    }
    if (x.size() != y.size())
        return false;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i] - y[i]) > 0.5)
            return false;
    return true;
}

/// What `lane` disagrees with the reference about. `only` restricts the fields
/// compared (the SwiftUI lane declares bindings and nothing else).
std::vector<Divergence> compare(const std::string& lane, const std::string& fixture,
                                const Projection& reference, const Projection& candidate,
                                const std::set<std::string>& only = {}) {
    std::vector<Divergence> out;
    std::set<std::string> anchors;
    for (const auto& [anchor, _] : reference)
        anchors.insert(anchor);
    for (const auto& [anchor, _] : candidate)
        anchors.insert(anchor);
    static const std::map<std::string, std::string> kEmpty;
    for (const auto& anchor : anchors) {
        const auto r = reference.find(anchor);
        const auto c = candidate.find(anchor);
        const auto& rf = r == reference.end() ? kEmpty : r->second;
        const auto& cf = c == candidate.end() ? kEmpty : c->second;
        std::set<std::string> fields;
        for (const auto& [field, _] : rf)
            fields.insert(field);
        for (const auto& [field, _] : cf)
            fields.insert(field);
        for (const auto& field : fields) {
            if (!only.empty() && only.count(field) == 0)
                continue;
            const auto rv = rf.count(field) ? rf.at(field) : std::string("<absent>");
            const auto cv = cf.count(field) ? cf.at(field) : std::string("<absent>");
            if (rv == cv)
                continue;
            if (field == "paint.bounds" && rf.count(field) && cf.count(field) &&
                bounds_close(rv, cv))
                continue;
            out.push_back({lane, fixture, anchor, field, rv, cv});
        }
    }
    return out;
}

struct AllowEntry {
    std::string lane, field, anchor, expected, actual, issue, reason;
    std::set<std::string> fixtures; ///< empty: every fixture
    std::size_t matched = 0;
};

std::vector<AllowEntry> load_allowlist() {
    const auto doc = choc::json::parse(
        read_file(fs::path(PULP_REPO_ROOT) / "test/fixtures/emitter-conformance/allowlist.json"));
    std::vector<AllowEntry> out;
    const auto gaps = doc["gaps"];
    for (uint32_t i = 0; i < gaps.size(); ++i) {
        const auto g = gaps[i];
        AllowEntry entry;
        const auto text = [&](const char* name) {
            return g.hasObjectMember(name) ? std::string(g[name].toString()) : std::string();
        };
        entry.lane = text("lane");
        entry.field = text("field");
        entry.anchor = text("anchor");
        entry.expected = text("expected");
        entry.actual = text("actual");
        entry.issue = text("issue");
        entry.reason = text("reason");
        if (g.hasObjectMember("fixtures"))
            for (uint32_t j = 0; j < g["fixtures"].size(); ++j)
                entry.fixtures.insert(std::string(g["fixtures"][j].toString()));
        out.push_back(std::move(entry));
    }
    return out;
}

bool full_match(const std::string& pattern, const std::string& text) {
    return pattern.empty() || std::regex_match(text, std::regex(pattern));
}

bool allows(const AllowEntry& entry, const Divergence& d) {
    return entry.lane == d.lane && full_match(entry.field, d.field) &&
           (entry.fixtures.empty() || entry.fixtures.count(d.fixture) != 0) &&
           full_match(entry.anchor, d.anchor) && full_match(entry.expected, d.expected) &&
           full_match(entry.actual, d.actual);
}

std::string describe(const Divergence& d) {
    return d.lane + " " + d.fixture + " " + d.anchor + " " + d.field + ": native '" + d.expected +
           "', " + d.lane + " '" + d.actual + "'";
}

std::vector<Divergence> all_divergences() {
    std::vector<Divergence> out;
    for (const auto& fixture : fixtures()) {
        INFO("fixture " << fixture.name);
        const auto ir = load(fixture);
        const StagedAssets assets(ir);
        const auto native = native_lane(ir, assets.dir());
        REQUIRE_FALSE(native.empty());
        for (auto&& d : compare("js", fixture.name, native, js_lane(ir, assets.dir())))
            out.push_back(std::move(d));
        for (auto&& d : compare("cpp", fixture.name, native, cpp_lane(fixture, ir, assets.dir())))
            out.push_back(std::move(d));
        // SwiftUI compares only the anchors that bind on at least one side.
        Projection native_bindings;
        for (const auto& [anchor, fields] : native)
            if (fields.count("binding"))
                native_bindings[anchor]["binding"] = fields.at("binding");
        for (auto&& d :
             compare("swiftui", fixture.name, native_bindings, swift_lane(ir), {"binding"}))
            out.push_back(std::move(d));
    }
    return out;
}

} // namespace

TEST_CASE("every emitter agrees with the native materializer or names the gap",
          "[view][import][emitter-conformance]") {
    auto allowlist = load_allowlist();
    const std::regex issue_url(R"(https://github\.com/Generous-Corp/pulp/(issues|pull)/[0-9]+)");
    for (const auto& entry : allowlist) {
        INFO("allowlist entry " << entry.lane << " " << entry.field);
        CHECK(std::regex_match(entry.issue, issue_url));
        CHECK_FALSE(entry.reason.empty());
        CHECK((entry.lane == "js" || entry.lane == "cpp" || entry.lane == "swiftui"));
    }

    const auto divergences = all_divergences();
    std::vector<std::string> unexplained;
    for (const auto& d : divergences) {
        bool explained = false;
        for (auto& entry : allowlist) {
            if (allows(entry, d)) {
                ++entry.matched;
                explained = true;
            }
        }
        if (!explained)
            unexplained.push_back(describe(d));
    }

    // PULP_CONFORMANCE_REPORT=<path> writes every divergence, explained or not,
    // as JSON: the list to start from when closing a gap or adding an entry.
    if (const char* path = std::getenv("PULP_CONFORMANCE_REPORT"); path && *path) {
        auto rows = choc::value::createEmptyArray();
        for (const auto& d : divergences) {
            auto row = choc::value::createObject("");
            row.addMember("lane", d.lane);
            row.addMember("fixture", d.fixture);
            row.addMember("anchor", d.anchor);
            row.addMember("field", d.field);
            row.addMember("native", d.expected);
            row.addMember("actual", d.actual);
            rows.addArrayElement(row);
        }
        std::ofstream(path) << choc::json::toString(rows, true);
    }

    std::ostringstream report;
    for (std::size_t i = 0; i < unexplained.size() && i < 60; ++i)
        report << "  " << unexplained[i] << "\n";
    if (unexplained.size() > 60)
        report << "  ... and " << unexplained.size() - 60 << " more\n";
    INFO(divergences.size() << " divergences, " << unexplained.size() << " unexplained:\n"
                            << report.str());
    CHECK(unexplained.empty());

    for (const auto& entry : allowlist) {
        INFO("stale allowlist entry (matches no divergence; delete it): "
             << entry.lane << " " << entry.field << " " << entry.issue);
        CHECK(entry.matched > 0);
    }
}

namespace {

IRNode theme_control(std::string anchor, std::string control, float left) {
    IRNode node;
    node.type = "frame";
    node.stable_anchor_id = std::move(anchor);
    node.style.position = "absolute";
    node.style.left = left;
    node.style.top = 10.5f;
    node.style.width = 90.0f;
    node.style.height = 16.0f;
    node.attributes["source_tag"] = "input";
    node.attributes["native_appearance"] = "auto";
    node.attributes["native_theme"] = "native";
    node.attributes["ua_control"] = std::move(control);
    // Chromium's CSS-edge snap of the box at DPR 2.
    node.attributes["browser_box_paint_left_px"] = std::to_string(static_cast<int>(left * 2));
    node.attributes["browser_box_paint_top_px"] = "20";
    node.attributes["browser_box_paint_right_px"] =
        std::to_string(static_cast<int>(left * 2) + 180);
    node.attributes["browser_box_paint_bottom_px"] = "54";
    node.attributes["browser_box_paint_dpr"] = "2";
    return node;
}

} // namespace

TEST_CASE("ui.js lowers theme-painted controls and snapped paint rects as the native lane does",
          "[view][import][emitter-conformance][form-controls]") {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = 640.0f;
    ir.root.style.height = 120.0f;

    auto range = theme_control("range", "range", 10.25f);
    range.attributes["ua_min"] = "0";
    range.attributes["ua_max"] = "1";
    range.attributes["ua_step"] = "0.001";
    range.attributes["ua_value"] = "0.25";
    range.attributes["ua_disabled"] = "1";
    range.attributes["ua_accent"] = "rgb(71, 180, 235)";
    auto checkbox = theme_control("checkbox", "checkbox", 120.5f);
    checkbox.attributes["ua_checked"] = "1";
    checkbox.attributes["ua_accent"] = "rgb(200, 30, 90)";
    checkbox.attributes["ua_color_scheme"] = "dark";
    auto radio = theme_control("radio", "radio", 230.0f);
    radio.attributes["ua_radio_group"] = "voice";
    // A rect no lane may apply: right of its own left edge is required.
    auto plain = theme_control("plain", "range", 340.0f);
    plain.attributes["native_theme"] = "author";
    plain.attributes["browser_box_paint_right_px"] = "10";
    // A theme-painted <button> keeps its label as a child.
    auto button = theme_control("button", "button", 450.0f);
    button.attributes["ua_disabled"] = "1";
    button.attributes["ua_color_scheme"] = "dark";
    IRNode label;
    label.type = "text";
    label.text_content = "Apply";
    label.stable_anchor_id = "button-label";
    label.style.position = "absolute";
    label.style.left = 4.0f;
    label.style.top = 2.0f;
    label.style.width = 40.0f;
    label.style.height = 12.0f;
    button.children.push_back(label);
    for (auto* node : {&range, &checkbox, &radio, &plain, &button})
        ir.root.children.push_back(*node);

    const StagedAssets assets(ir);
    const auto native = native_lane(ir, assets.dir());
    const auto js = js_lane(ir, assets.dir());
    REQUIRE(native.at("range").at("kind") == "pulp::view::RangeSlider");
    REQUIRE(native.at("radio").at("state.control") == "radio");
    REQUIRE(native.at("button").at("kind") == "pulp::view::ButtonSurface");
    REQUIRE(native.at("button").at("state.scheme") == "dark");
    REQUIRE(native.at("button-label").at("parent") == "button");
    REQUIRE(native.at("range").count("paint.captured") == 1);
    REQUIRE(native.at("plain").count("paint.captured") == 0);

    std::ostringstream found;
    for (const auto& d : compare("js", "synthetic", native, js))
        found << describe(d) << "\n";
    INFO(found.str());
    CHECK(found.str().empty());
}

TEST_CASE("ui.js leaves a theme-painted control it has no case for as a plain frame",
          "[view][import][emitter-conformance][form-controls]") {
    // A control kind the ui.js lowering does not handle (a colour picker, a
    // kind nobody records yet) must not fall through to another widget: it
    // stays a frame, as it does in the native lane until that lane lowers it.
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = 400.0f;
    ir.root.style.height = 60.0f;
    for (const char* control : {"color", "made-up"}) {
        auto node = theme_control(control, control, 10.0f + 100.0f * ir.root.children.size());
        node.children.push_back(IRNode{});
        node.children.back().type = "frame";
        node.children.back().stable_anchor_id = std::string(control) + "-face";
        node.children.back().style.width = 20.0f;
        node.children.back().style.height = 10.0f;
        ir.root.children.push_back(std::move(node));
    }

    const auto js = generate_pulp_js(ir, CodeGenOptions{});
    CHECK(js.find("createCheckbox(") == std::string::npos);
    CHECK(js.find("createRangeSlider(") == std::string::npos);
    CHECK(js.find("setUaSkin(") == std::string::npos);

    const StagedAssets assets(ir);
    const auto native = native_lane(ir, assets.dir());
    const auto lane = js_lane(ir, assets.dir());
    for (const char* control : {"color", "made-up"}) {
        INFO(control);
        REQUIRE(lane.count(control) == 1);
        CHECK(lane.at(control).at("kind") == "pulp::view::View");
        CHECK(lane.at(control).count("state.skin") == 0);
        CHECK(native.at(control).at("kind") == "pulp::view::View");
    }
}

TEST_CASE("the conformance comparison reports planted divergences",
          "[view][import][emitter-conformance]") {
    Projection reference;
    reference["a"] = {{"present", "yes"},
                      {"kind", "pulp::view::RangeSlider"},
                      {"paint.bounds", "10.000,10.000,90.000,16.000"},
                      {"binding", "param:gain"}};
    reference["b"] = {{"present", "yes"}, {"kind", "pulp::view::View"}};

    SECTION("identical projections agree") {
        CHECK(compare("js", "f", reference, reference).empty());
    }
    SECTION("paint bounds within half a pixel agree, beyond it diverge") {
        auto near = reference;
        near["a"]["paint.bounds"] = "10.400,10.000,90.000,16.000";
        CHECK(compare("js", "f", reference, near).empty());
        auto far = reference;
        far["a"]["paint.bounds"] = "11.000,10.000,90.000,16.000";
        REQUIRE(compare("js", "f", reference, far).size() == 1);
    }
    SECTION("a wrong kind, a lost binding and a missing node are each reported") {
        auto candidate = reference;
        candidate["a"]["kind"] = "pulp::view::View";
        candidate["a"].erase("binding");
        candidate.erase("b");
        const auto found = compare("js", "f", reference, candidate);
        std::set<std::string> fields;
        for (const auto& d : found)
            fields.insert(d.anchor + "." + d.field);
        CHECK(fields == std::set<std::string>{"a.kind", "a.binding", "b.present", "b.kind"});
    }
    SECTION("an allowlist entry only explains what it names") {
        AllowEntry entry;
        entry.lane = "js";
        entry.field = "kind";
        entry.expected = "pulp::view::RangeSlider";
        entry.fixtures = {"f"};
        CHECK(allows(entry, {"js", "f", "a", "kind", "pulp::view::RangeSlider", "x"}));
        CHECK_FALSE(allows(entry, {"cpp", "f", "a", "kind", "pulp::view::RangeSlider", "x"}));
        CHECK_FALSE(allows(entry, {"js", "g", "a", "kind", "pulp::view::RangeSlider", "x"}));
        CHECK_FALSE(allows(entry, {"js", "f", "a", "kind", "pulp::view::Checkbox", "x"}));
    }
}
