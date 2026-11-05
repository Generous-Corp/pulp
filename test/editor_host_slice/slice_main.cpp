// The editor-host slice harness, linked unchanged into all three executables.
//
//   pulp-slice-<kind> <out-dir>
//
// 1. Renders one second of fixed stimulus through GainMeterProcessor and
//    writes the raw float output to <out-dir>/<kind>.f32. The ctest that runs
//    all three compares those files byte for byte.
// 2. When this binary links an editor (`slice_editor_factory()` is non-null),
//    drives the editor-host contract in process: open, attach to a real
//    NSView, gesture, capture, resize, close, reopen, host-side set, editor
//    plus inspector, Space forwarding and host invalidation. Then it renders
//    again with the editor attached and requires the same bytes.
//
// Exit code 0 means every check passed; each failure prints one line.

#include "gain_meter_processor.hpp"
#include "host_window_mac.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/editor_host.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/format/host_parameter_edit.hpp>
#include <pulp/state/store.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef SLICE_KIND
#error "SLICE_KIND must name this executable's editor kind"
#endif

namespace pulp::slice {
int slice_editor_live_instances();
}

using namespace pulp;
using namespace pulp::slice;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[%s] FAIL: %s\n", SLICE_KIND, what);
    } else {
        std::printf("[%s] ok: %s\n", SLICE_KIND, what);
    }
}

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;
constexpr int kBlocks = 94; // just over one second

/// One second of fixed stereo stimulus through the processor, as raw floats.
/// `between_blocks` runs on the main thread after every block, the way a host
/// ticks an open editor while audio runs.
std::vector<float> render(format::HeadlessHost& host, const std::function<void()>& between_blocks) {
    host.prepare(kSampleRate, kBlock, 2, 2);
    audio::Buffer<float> in(2, kBlock), out(2, kBlock);
    std::vector<float> result;
    result.reserve(static_cast<std::size_t>(kBlocks) * kBlock * 2);
    std::uint64_t frame = 0;
    for (int b = 0; b < kBlocks; ++b) {
        for (int i = 0; i < kBlock; ++i, ++frame) {
            const double t = static_cast<double>(frame) / kSampleRate;
            in.view().channel_ptr(0)[i] =
                static_cast<float>(0.8 * std::sin(2.0 * M_PI * 440.0 * t));
            in.view().channel_ptr(1)[i] =
                static_cast<float>(0.5 * std::sin(2.0 * M_PI * 660.0 * t));
        }
        const float* in_ptrs[2] = {in.view().channel_ptr(0), in.view().channel_ptr(1)};
        audio::BufferView<const float> in_const(in_ptrs, 2, kBlock);
        auto out_view = out.view();
        host.process(out_view, in_const);
        for (int ch = 0; ch < 2; ++ch)
            result.insert(result.end(), out_view.channel_ptr(ch),
                          out_view.channel_ptr(ch) + kBlock);
        if (between_blocks)
            between_blocks();
    }
    return result;
}

/// The host side of the gesture path, as "B1 V1(0.75) E1".
struct HostLog {
    std::vector<std::string> entries;
    std::string stream() const {
        std::string s;
        for (const auto& e : entries)
            s += (s.empty() ? "" : " ") + e;
        return s;
    }
    void clear() {
        entries.clear();
    }
};

class SliceContext final : public format::EditorContext {
  public:
    SliceContext(state::StateStore& store, PeakMeter& meter) : port_(store), visuals_(meter) {}
    format::ParamPort& params() override {
        return port_;
    }
    format::VisualPort* visuals() override {
        return &visuals_;
    }
    bool request_resize(const format::EditorSize&) override {
        ++resize_requests;
        return true;
    }
    bool host_action(const format::HostActionRequest&) override {
        return false;
    }
    format::EditorHostKind host_kind() const override {
        return format::EditorHostKind::plugin;
    }
    format::StateStoreParamPort& port() {
        return port_;
    }
    int resize_requests = 0;

  private:
    format::StateStoreParamPort port_;
    PeakMeterVisualPort visuals_;
};

float observe_number(format::Editor& editor, const char* key) {
    std::string reply;
    if (!editor.automation() || !editor.automation()->observe("", reply))
        return -1.0f;
    const auto at = reply.find(key);
    return at == std::string::npos
               ? -1.0f
               : std::strtof(reply.c_str() + at + std::strlen(key) + 2, nullptr);
}

bool write_file(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void exercise_editor(format::HeadlessHost& host, format::EditorFactory& factory,
                     const std::filesystem::path& out_dir) {
    auto& processor = *host.processor_as<GainMeterProcessor>();
    auto& store = host.state();

    HostLog log;
    format::HostParameterEditBridge bridge(
        store, {[&](state::ParamID id) { log.entries.push_back("B" + std::to_string(id)); },
                [&](state::ParamID id, float normalized, float) {
                    char v[16];
                    std::snprintf(v, sizeof v, "%.2f", normalized);
                    log.entries.push_back("V" + std::to_string(id) + "(" + v + ")");
                },
                [&](state::ParamID id) { log.entries.push_back("E" + std::to_string(id)); }});

    host_app_init();
    void* window = host_window_create(480, 320);
    void* parent = host_window_content_view(window);
    const int baseline_subviews = host_view_subview_count(parent);

    check(factory.supports(format::EditorRole::editor), "factory supports the editor role");
    check(!factory.supports(format::EditorRole::settings), "factory declines the settings role");

    // ── open, size before attach, attach, gesture ──
    SliceContext context(store, processor.meter());
    {
        auto editor = factory.create(processor, context, format::EditorRole::editor);
        check(editor != nullptr, "create() returns an editor");
        if (!editor)
            return;
        check(editor->supports_parent(format::NativeParentKind::cocoa_view),
              "editor supports an NSView parent");
        const auto initial = editor->size();
        check(editor->set_size(initial), "set_size before attach is accepted");
        check(editor->attach({format::NativeParentKind::cocoa_view, parent, 0}),
              "attach to a real NSView");
        check(host_view_subview_count(parent) > baseline_subviews, "attach added a native subview");
        for (int i = 0; i < 3; ++i) {
            editor->idle();
            host_pump_events();
        }

        std::string error;
        log.clear();
        check(editor->automation() && editor->automation()->inject_input("drag -50", error),
              "inject a knob drag");
        const float after_drag = store.get_normalized(kGainParam);
        const std::string stream = log.stream();
        std::printf("[%s] drag stream: %s (value %.4f)\n", SLICE_KIND, stream.c_str(), after_drag);
        check(stream.rfind("B1 ", 0) == 0, "the drag opens one host gesture first");
        check(stream.size() > 3 && stream.compare(stream.size() - 3, 3, " E1") == 0,
              "the drag closes the host gesture last");
        check(stream.find("B1", 1) == std::string::npos, "exactly one begin");
        check(after_drag > kGainDefault + 0.05f, "dragging up raised the gain");
#if SLICE_EXACT_DRAG
        check(std::fabs(after_drag - 0.75f) < 1e-6f, "the drag lands on exactly 0.75");
        char last[32];
        std::snprintf(last, sizeof last, "V1(0.75) E1");
        check(stream.size() >= std::strlen(last) &&
                  stream.compare(stream.size() - std::strlen(last), std::strlen(last), last) == 0,
              "the host sees set(0.75) immediately before end");
#endif
        check(std::fabs(observe_number(*editor, "\"" SLICE_VALUE_KEY "\"") - after_drag) < 1e-4f,
              "the editor shows the value the host holds");
        check(context.port().open_gestures(kGainParam) == 0 && store.open_gesture_count() == 0,
              "no gesture is left open after the drag");

        std::vector<std::uint8_t> png;
        check(editor->automation()->capture_png(png) && png.size() > 8,
              "capture_png produces an image");
        write_file(out_dir / (std::string(SLICE_KIND) + "-open.png"), png.data(), png.size());
#ifdef SLICE_GOLDEN_PNG
        check(png == read_file(SLICE_GOLDEN_PNG),
              "the frame at gain 0.75 equals the module's golden image byte for byte");
#endif
#if SLICE_EMPTY_DOCUMENT
        {
            // The negative control: the same comparison must reject a frame of
            // the module's empty document, and accept the knob document again
            // once it is back.
            std::string error;
            std::vector<std::uint8_t> empty;
            check(editor->automation()->inject_input("document empty.rml", error) &&
                      editor->automation()->capture_png(empty) && empty.size() > 8,
                  "an empty document renders");
            check(empty != read_file(SLICE_GOLDEN_PNG),
                  "an empty document's frame does not equal the golden image");
            std::vector<std::uint8_t> again;
            check(editor->automation()->inject_input("document knob_led.rml", error) &&
                      editor->automation()->capture_png(again) &&
                      again == read_file(SLICE_GOLDEN_PNG),
                  "the knob document's frame equals the golden image again");
        }
#endif

        // ── resize ──
        check(!editor->set_size({10, 10, 1.0}), "an undersized set_size is refused");
        host_window_resize(window, 320, 200);
        check(editor->set_size({320, 200, 1.0}), "set_size to 320x200 is accepted");
        check(editor->size().width == 320 && editor->size().height == 200,
              "size() reports the new size");
        std::vector<std::uint8_t> resized;
        check(editor->automation()->capture_png(resized) && resized != png,
              "the resized frame differs from the original");
        write_file(out_dir / (std::string(SLICE_KIND) + "-resized.png"), resized.data(),
                   resized.size());

        check(editor->offer_key({format::EditorKey::space, U' ', 0, true, false}) ==
                  format::KeyDisposition::forward,
              "Space with no focused field is forwarded to the host");
#if SLICE_TEXT_FIELD
        {
            std::string error;
            check(editor->automation()->inject_input("focus text", error),
                  "the editor's text field takes focus");
            check(editor->offer_key({format::EditorKey::space, U' ', 0, true, false}) ==
                      format::KeyDisposition::consumed,
                  "Space is consumed when a text field has focus");
            std::string reply;
            check(editor->automation()->observe("state", reply) &&
                      reply.find("\"text\":\" \"") != std::string::npos,
                  "the focused text field received the Space");
            check(editor->automation()->inject_input("blur text", error),
                  "the editor's text field gives up focus");
            check(editor->offer_key({format::EditorKey::space, U' ', 0, true, false}) ==
                      format::KeyDisposition::forward,
                  "Space is forwarded again once the field loses focus");
        }
#endif

        // ── close ──
        editor->detach();
        check(host_view_subview_count(parent) == baseline_subviews,
              "detach removed every subview it added");
    }
    check(slice_editor_live_instances() == 0, "closing destroyed the editor's UI");
    context.port().release_all();
    check(store.open_gesture_count() == 0, "no gesture survives closing the editor");

    // ── reopen; value survives; host-side set reaches the editor ──
    {
        auto editor = factory.create(processor, context, format::EditorRole::editor);
        check(editor && editor->attach({format::NativeParentKind::cocoa_view, parent, 0}),
              "reopen and attach again");
        if (!editor)
            return;
        const float held = store.get_normalized(kGainParam);
        check(std::fabs(observe_number(*editor, "\"" SLICE_VALUE_KEY "\"") - held) < 1e-4f,
              "the reopened editor shows the surviving value");
#if SLICE_HAS_LED
        std::string led;
        editor->automation()->observe("led", led);
        check(led == "on", "the LED is on above 0.5");
#endif
        log.clear();
        {
            format::ScopedHostParameterWrite host_write;
            store.set_value(kGainParam, 0.2f);
        }
        editor->idle();
        check(log.entries.empty(), "a host-side set is not echoed back as an editor edit");
        check(std::fabs(observe_number(*editor, "\"" SLICE_VALUE_KEY "\"") - 0.2f) < 1e-4f,
              "a host-side set of 0.2 reaches the editor");
#if SLICE_HAS_LED
        editor->automation()->observe("led", led);
        check(led == "off", "the LED turns off after one frame at 0.2");
#endif

        // ── editor and inspector together, over separate ports ──
        SliceContext inspector_context(store, processor.meter());
        auto inspector =
            factory.create(processor, inspector_context, format::EditorRole::inspector);
        check(inspector != nullptr, "an inspector opens beside the editor");
        if (inspector) {
            std::string error;
            check(slice_editor_live_instances() == 2, "two editor instances are open at once");
            inspector->automation()->inject_input("drag -40", error);
            editor->idle();
            const float now = store.get_normalized(kGainParam);
            check(now > 0.2f + 0.05f, "the inspector's drag moved the parameter");
            check(std::fabs(observe_number(*editor, "\"" SLICE_VALUE_KEY "\"") - now) < 1e-4f,
                  "the editor follows the inspector's edit");
            inspector.reset();
        }
        editor->detach();
    }

    // ── host-driven invalidation ──
    {
        void* doomed = host_window_create(200, 120);
        auto editor = factory.create(processor, context, format::EditorRole::editor);
        if (editor) {
            editor->attach(
                {format::NativeParentKind::cocoa_view, host_window_content_view(doomed), 0});
            editor->invalidate();
            host_window_destroy(doomed);
            editor.reset();
            check(true, "invalidate then destroy after the host window is gone");
        }
    }
    check(slice_editor_live_instances() == 0, "every editor UI is released");
    context.port().release_all();
    check(store.open_gesture_count() == 0, "no gesture is left open at exit");
    host_window_destroy(window);
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path out_dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(out_dir);

    format::HeadlessHost host(GainMeterProcessor::create);
    check(host.valid(), "the processor is created");
    if (!host.valid())
        return 1;
    auto* processor = host.processor_as<GainMeterProcessor>();

    // Audio with no editor ever opened.
    const auto reference = render(host, {});
    check(write_file(out_dir / (std::string(SLICE_KIND) + ".f32"), reference.data(),
                     reference.size() * sizeof(float)),
          "write the rendered audio");

    format::EditorFactory* factory = processor->editor_factory();
#if SLICE_EXPECT_EDITOR
    check(factory != nullptr, "this binary links an editor factory");
    if (!factory)
        return 1;
    exercise_editor(host, *factory, out_dir);

    // Audio again, at the same gain, with an editor attached and ticking
    // between blocks.
    {
        format::ScopedHostParameterWrite host_write;
        host.state().set_value(kGainParam, kGainDefault);
    }
    void* window = host_window_create(320, 200);
    SliceContext context(host.state(), processor->meter());
    auto editor = factory->create(*processor, context, format::EditorRole::editor);
    check(editor && editor->attach({format::NativeParentKind::cocoa_view,
                                    host_window_content_view(window), 0}),
          "attach an editor for the second render");
    const auto with_editor = render(host, [&] {
        if (editor)
            editor->idle();
    });
    check(with_editor == reference, "audio with an editor attached is byte-identical");
    if (editor)
        editor->detach();
    editor.reset();
    host_window_destroy(window);
#else
    check(factory == nullptr, "this binary links no editor factory");
#endif

    std::printf("[%s] %s\n", SLICE_KIND, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
