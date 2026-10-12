// Binding imported level and series displays through the audio-widget provider.
//
// NativeImportBindingContext carries two generations of display binding: the
// provider-neutral bind_level_display / bind_series_display, which the importer
// calls directly, and the deprecated bind_meter / bind_waveform_display, which
// only the provider can reach because only it knows the concrete classes. A
// plugin that overrides either generation must still be bound.

#include <pulp/view/data_display.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/register_audio_widgets.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::view;

namespace {

DesignIR bound_displays_ir() {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "displays";
    ir.root.style.width = 160.0f;
    ir.root.style.height = 24.0f;
    ir.root.layout.direction = LayoutDirection::row;

    IRNode meter;
    meter.type = "frame";
    meter.stable_anchor_id = "level-meter";
    meter.style.width = 80.0f;
    meter.style.height = 24.0f;
    meter.audio_widget = AudioWidgetType::meter;
    meter.attributes["pulpRouteId"] = "displays:level";
    meter.attributes["pulpMeterSource"] = "output";
    meter.attributes["pulpMeterChannel"] = "left";
    meter.attributes["pulpMeterValueKey"] = "rms";

    IRNode waveform;
    waveform.type = "frame";
    waveform.stable_anchor_id = "osc-waveform";
    waveform.style.width = 80.0f;
    waveform.style.height = 24.0f;
    waveform.audio_widget = AudioWidgetType::waveform;
    waveform.attributes["pulpRouteId"] = "displays:waveform";
    waveform.attributes["pulpParamKey"] = "osc.shape";
    waveform.attributes["pulpWaveformShape"] = "saw";

    ir.root.children.push_back(std::move(meter));
    ir.root.children.push_back(std::move(waveform));
    return ir;
}

struct Calls {
    std::vector<std::string> level;
    std::vector<std::string> series;
    std::vector<std::string> meter;
    std::vector<std::string> waveform;
};

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

// A plugin written before the provider-neutral virtuals existed.
class ConcreteOnlyContext final : public NativeImportBindingContext {
public:
    void bind_meter(Meter& meter, const NativeImportMeterBindingDescriptor& d) override {
        CHECK(ViewInspector::type_name(meter) == "Meter");
        calls.meter.emplace_back(d.channel);
    }
    void bind_waveform_display(WaveformView& waveform,
                               const NativeImportWaveformBindingDescriptor& d) override {
        CHECK(ViewInspector::type_name(waveform) == "WaveformView");
        calls.waveform.emplace_back(d.param_key);
    }
    Calls calls;
};

// A plugin that overrides both generations is called through both.
class BothContext final : public NativeImportBindingContext {
public:
    void bind_level_display(View&, LevelDisplay&,
                            const NativeImportMeterBindingDescriptor& d) override {
        calls.level.emplace_back(d.channel);
    }
    void bind_series_display(View&, SeriesDisplay&,
                             const NativeImportWaveformBindingDescriptor& d) override {
        calls.series.emplace_back(d.param_key);
    }
    void bind_meter(Meter&, const NativeImportMeterBindingDescriptor& d) override {
        calls.meter.emplace_back(d.channel);
    }
    void bind_waveform_display(WaveformView&,
                               const NativeImportWaveformBindingDescriptor& d) override {
        calls.waveform.emplace_back(d.param_key);
    }
    Calls calls;
};

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

class InterfaceOnlyContext final : public NativeImportBindingContext {
public:
    void bind_level_display(View& view, LevelDisplay&,
                            const NativeImportMeterBindingDescriptor& d) override {
        CHECK(ViewInspector::type_name(view) == "Meter");
        calls.level.emplace_back(d.channel);
    }
    void bind_series_display(View& view, SeriesDisplay&,
                             const NativeImportWaveformBindingDescriptor& d) override {
        CHECK(ViewInspector::type_name(view) == "WaveformView");
        calls.series.emplace_back(d.param_key);
    }
    Calls calls;
};

template <typename Context>
Calls bind_with(Context& ctx) {
    register_audio_widgets();
    const auto ir = bound_displays_ir();
    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root != nullptr);
    std::vector<ImportDiagnostic> diagnostics;
    bind_native_view_tree(*root, ir, ctx, {.diagnostics_out = &diagnostics});
    for (const auto& d : diagnostics)
        CHECK(d.code != "native-binding-not-applied");
    return ctx.calls;
}

} // namespace

TEST_CASE("a context overriding only the deprecated concrete binders is still bound",
          "[view][import][data-display][binding]") {
    ConcreteOnlyContext ctx;
    const auto calls = bind_with(ctx);
    CHECK(calls.meter == std::vector<std::string>{"left"});
    CHECK(calls.waveform == std::vector<std::string>{"osc.shape"});
}

TEST_CASE("a context overriding only the provider-neutral binders is bound to the provider's widgets",
          "[view][import][data-display][binding]") {
    InterfaceOnlyContext ctx;
    const auto calls = bind_with(ctx);
    CHECK(calls.level == std::vector<std::string>{"left"});
    CHECK(calls.series == std::vector<std::string>{"osc.shape"});
}

TEST_CASE("a context overriding both binder generations is called through each once",
          "[view][import][data-display][binding]") {
    BothContext ctx;
    const auto calls = bind_with(ctx);
    CHECK(calls.level == std::vector<std::string>{"left"});
    CHECK(calls.series == std::vector<std::string>{"osc.shape"});
    CHECK(calls.meter == std::vector<std::string>{"left"});
    CHECK(calls.waveform == std::vector<std::string>{"osc.shape"});
}
