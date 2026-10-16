// The importer and the script bridge without the audio-widget provider.
//
// This executable links pulp::design-import and pulp::view-script and nothing
// that carries pulp::view-audio, so no level, series or bin display factory is
// registered. It must still link (GNU ld resolves single-pass, so a stray
// reference to Meter, WaveformView or SpectrumView fails here first) and the
// two extracted units must degrade to diagnosable placeholders: the importer
// emits `data-display-unavailable` and reports the binding as not applied, and
// the bridge's create functions raise a script error.

#include <pulp/state/store.hpp>
#include <pulp/view/data_display.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/inspector_facts.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::view;

namespace {

IRNode display_node(std::string anchor, AudioWidgetType kind) {
    IRNode node;
    node.type = "frame";
    node.stable_anchor_id = std::move(anchor);
    node.style.width = 80.0f;
    node.style.height = 24.0f;
    node.audio_widget = kind;
    return node;
}

DesignIR displays_ir() {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "displays";
    ir.root.style.width = 240.0f;
    ir.root.style.height = 24.0f;
    ir.root.layout.direction = LayoutDirection::row;

    auto meter = display_node("level-meter", AudioWidgetType::meter);
    meter.attributes["value"] = "0.5";
    meter.attributes["pulpRouteId"] = "displays:level";
    meter.attributes["pulpMeterSource"] = "output";
    meter.attributes["pulpMeterChannel"] = "left";
    meter.attributes["pulpMeterValueKey"] = "rms";

    auto waveform = display_node("osc-waveform", AudioWidgetType::waveform);
    waveform.attributes["pulpRouteId"] = "displays:waveform";
    waveform.attributes["pulpParamKey"] = "osc.shape";
    waveform.attributes["pulpWaveformShape"] = "saw";

    ir.root.children.push_back(std::move(meter));
    ir.root.children.push_back(std::move(waveform));
    ir.root.children.push_back(display_node("spectrum", AudioWidgetType::spectrum));
    return ir;
}

View* find_anchor(View& root, std::string_view anchor) {
    if (root.anchor_id() == anchor)
        return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* hit = find_anchor(*root.child_at(i), anchor))
            return hit;
    return nullptr;
}

std::size_t count_code(const std::vector<ImportDiagnostic>& diagnostics, std::string_view code) {
    return static_cast<std::size_t>(
        std::count_if(diagnostics.begin(), diagnostics.end(),
                      [&](const ImportDiagnostic& d) { return d.code == code; }));
}

// Overrides only the provider-neutral virtuals, as a plugin written against
// view-core does.
class InterfaceOnlyContext final : public NativeImportBindingContext {
  public:
    void bind_level_display(View&, LevelDisplay&,
                            const NativeImportMeterBindingDescriptor& descriptor) override {
        level_channels.emplace_back(descriptor.channel);
    }
    void bind_series_display(View&, SeriesDisplay&,
                             const NativeImportWaveformBindingDescriptor& descriptor) override {
        series_params.emplace_back(descriptor.param_key);
    }
    std::vector<std::string> level_channels;
    std::vector<std::string> series_params;
};

// A provider that is not pulp::view-audio: proves the importer reaches a display
// only through the view-core facets.
class StubLevel final : public View, public LevelDisplay {
  public:
    void set_levels(float, float) override {}
    void set_level_orientation(Orientation) override {}
    void set_level_fill(std::optional<pulp::canvas::Color>,
                        std::vector<pulp::canvas::Color>) override {}
    void clear_level_fill() override {}
    void set_level_bar_ratio(float) override {}
    void set_level_render_style(WidgetRenderStyle) override {}
};

class StubSeries final : public View, public SeriesDisplay {
  public:
    void set_series(const float*, std::size_t) override {}
    void set_series_preview(std::string_view) override {}
};

std::unique_ptr<View> make_stub_level() {
    return std::make_unique<StubLevel>();
}
std::unique_ptr<View> make_stub_series() {
    return std::make_unique<StubSeries>();
}

struct ClearDisplaysOnExit {
    ~ClearDisplaysOnExit() {
        clear_data_displays();
    }
};

} // namespace

TEST_CASE("without the audio provider, imported data displays are diagnosed placeholders",
          "[view][import][data-display][link-floor]") {
    // Nothing in this image registers a display, so every lookup misses.
    REQUIRE_FALSE(has_data_display("pulp.level"));
    REQUIRE_FALSE(has_data_display("pulp.series"));
    REQUIRE_FALSE(has_data_display("pulp.bins"));

    const auto ir = displays_ir();
    std::vector<ImportDiagnostic> diagnostics;
    auto root = build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
    REQUIRE(root != nullptr);
    CHECK(count_code(diagnostics, "data-display-unavailable") == 3);

    for (const char* anchor : {"level-meter", "osc-waveform", "spectrum"}) {
        INFO(anchor);
        auto* view = find_anchor(*root, anchor);
        REQUIRE(view != nullptr);
        CHECK(view_type_name(*view) == "View");
        CHECK(dynamic_cast<LevelDisplay*>(view) == nullptr);
        CHECK(dynamic_cast<SeriesDisplay*>(view) == nullptr);
        CHECK(dynamic_cast<BinDisplay*>(view) == nullptr);
    }

    // The binding metadata is reported, not silently dropped.
    InterfaceOnlyContext ctx;
    std::vector<ImportDiagnostic> binding_diagnostics;
    bind_native_view_tree(*root, ir, ctx, {.diagnostics_out = &binding_diagnostics});
    CHECK(ctx.level_channels.empty());
    CHECK(ctx.series_params.empty());
    CHECK(count_code(binding_diagnostics, "native-binding-not-applied") == 2);
}

TEST_CASE("without the audio provider, the bridge's display factories raise a script error",
          "[view][script][data-display][link-floor]") {
    REQUIRE_FALSE(has_data_display("pulp.level"));
    ScriptEngine engine;
    View root;
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store);

    for (const char* call : {"createMeter('m', 'vertical', '')", "createWaveform('w', '')",
                             "createSpectrum('s', '')"}) {
        INFO(call);
        const auto result = engine.evaluate(std::string("(function(){ try { ") + call +
                                            "; return 'created'; } catch (e) { return 'error: ' + "
                                            "String(e && e.message ? e.message : e); } })()");
        REQUIRE(result.isString());
        const std::string text(result.getString());
        CHECK(text.rfind("error: ", 0) == 0);
        CHECK(text.find("audio widget unavailable") != std::string::npos);
    }
    CHECK(root.child_count() == 0);
}

TEST_CASE("an interface-only binding context is bound through any registered provider",
          "[view][import][data-display][link-floor]") {
    ClearDisplaysOnExit cleanup;
    register_data_display({"pulp.level", &make_stub_level, {}});
    register_data_display({"pulp.series", &make_stub_series, {}});

    const auto ir = displays_ir();
    std::vector<ImportDiagnostic> diagnostics;
    auto root = build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
    REQUIRE(root != nullptr);
    // The spectrum has no provider here; the other two resolve to the stubs.
    CHECK(count_code(diagnostics, "data-display-unavailable") == 1);
    REQUIRE(dynamic_cast<StubLevel*>(find_anchor(*root, "level-meter")) != nullptr);
    REQUIRE(dynamic_cast<StubSeries*>(find_anchor(*root, "osc-waveform")) != nullptr);

    InterfaceOnlyContext ctx;
    std::vector<ImportDiagnostic> binding_diagnostics;
    bind_native_view_tree(*root, ir, ctx, {.diagnostics_out = &binding_diagnostics});
    CHECK(ctx.level_channels == std::vector<std::string>{"left"});
    CHECK(ctx.series_params == std::vector<std::string>{"osc.shape"});
    CHECK(count_code(binding_diagnostics, "native-binding-not-applied") == 0);
}
