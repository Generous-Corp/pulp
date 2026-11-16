// test_native_surface_vst3.mm - the metal-probe editor (a native Metal
// surface) opened through the VST3 contract view, in an offscreen window.
//
// A PulpVst3Processor over the probe processor hands out the ContractPlugView
// a host receives from createView(). The test plays the host's part: it
// attaches the view to an NSView, resizes that NSView and calls onSize, and
// removes the view. What the host sees is asserted by the checks shared with
// the other formats (support/native_surface_format_checks.hpp). Skips, saying
// so, without a Metal device. Compiled with ARC.

#include <catch2/catch_test_macros.hpp>

#include "support/native_surface_format_checks.hpp"
#include "support/recording_contract_editor.hpp"

#include "metal_probe_processor.hpp"

#include <pulp/format/editor_package.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/vst3_contract_view.hpp>
#include <pulp/state/store.hpp>

#include <pluginterfaces/vst/ivsthostapplication.h>

using namespace pulp::test_support;
using pulp::format::vst3::ContractPlugView;
using pulp::format::vst3::PulpVst3Processor;

namespace {

struct InstallHook {
    InstallHook() {
        pulp::format::install_plugin_editor_factory_hook(&pulp_native_metal_probe::editor_factory);
    }
} g_install_hook;

class HostApp final : public Steinberg::Vst::IHostApplication {
  public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        name[0] = 0;
        return Steinberg::kResultTrue;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID,
                                                 void** obj) override {
        if (obj)
            *obj = nullptr;
        return Steinberg::kNotImplemented;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {
        return 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        return 1;
    }
};

} // namespace

// The probe plug-in has an editor factory, so createView() never takes the
// create_view() path; the symbol is only here to link.
namespace pulp::format::vst3 {
Steinberg::IPlugView* make_plug_view(Processor&, state::StateStore&, runtime::AliveToken::Handle) {
    return nullptr;
}
} // namespace pulp::format::vst3

TEST_CASE("VST3 gives a native-surface editor its size, first frame, resize and reopen",
          "[native-surface][vst3][editor-host][format-host]") {
    if (!metal_device_available())
        SKIP("no Metal device on this host; the VST3 native surface was not exercised");
    EditorEnvironment env;
    MainLoop loop;
    HostApp host_app;
    PulpVst3Processor processor(pulp_native_metal_probe::create_metal_probe);
    REQUIRE(processor.initialize(&host_app) == Steinberg::kResultOk);

    ContractPlugView* view = nullptr;
    NSView* host_view = nil; // the NSView a VST3 host owns and hands the plug-in

    SurfaceFormatDriver driver;
    driver.open = [&](NSWindow* window, float gain) -> NSView* {
        processor.setParamNormalized(pulp_native_metal_probe::kGain, gain);
        auto* raw = processor.createView(Steinberg::Vst::ViewType::kEditor);
        view = dynamic_cast<ContractPlugView*>(raw);
        REQUIRE(view != nullptr);
        Steinberg::ViewRect rect;
        REQUIRE(view->getSize(&rect) == Steinberg::kResultTrue);
        host_view =
            [[NSView alloc] initWithFrame:NSMakeRect(0, 0, rect.getWidth(), rect.getHeight())];
        [window.contentView addSubview:host_view];
        if (view->attached((__bridge void*)host_view, Steinberg::kPlatformTypeNSView) !=
            Steinberg::kResultTrue) {
            view->release();
            view = nullptr;
            [host_view removeFromSuperview];
            host_view = nil;
            return nil;
        }
        return host_view;
    };
    driver.editor = [&]() -> pulp::format::Editor* { return view ? view->editor() : nullptr; };
    driver.host_resize = [&](std::uint32_t width, std::uint32_t height) {
        // The host sizes its view, then tells the plug-in.
        [host_view setFrameSize:NSMakeSize(width, height)];
        Steinberg::ViewRect rect(0, 0, static_cast<Steinberg::int32>(width),
                                 static_cast<Steinberg::int32>(height));
        return view->onSize(&rect) == Steinberg::kResultTrue;
    };
    driver.reported_size = [&] {
        Steinberg::ViewRect rect;
        view->getSize(&rect);
        return pulp::format::EditorSize{static_cast<std::uint32_t>(rect.getWidth()),
                                        static_cast<std::uint32_t>(rect.getHeight()), 1.0};
    };
    driver.close = [&] {
        REQUIRE(view->removed() == Steinberg::kResultOk);
        view->release();
        view = nullptr;
    };
    driver.release = [&] {
        [host_view removeFromSuperview];
        host_view = nil;
    };
    driver.run_loop = [&](double seconds) { loop.run_for(seconds); };

    check_native_surface_lifecycle(driver);

    processor.terminate();
}
