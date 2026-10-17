#include <pulp/view/design_import.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/screenshot.hpp>

#include <choc/text/choc_JSON.h>

#if defined(PULP_HAS_SKIA)
#include <pulp/canvas/canvas.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/headless_surface.hpp>
#include "render_observer_private.hpp"
#include <pulp/render/skia_surface.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#endif

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
                 "[--interaction-log <json>] [--backend skia|gpu] "
                 "[--render-receipt <json>] [--gpu-fault <fault>]\n";
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
    if (comma == std::string_view::npos)
        return std::nullopt;
    try {
        const std::string xs{text.substr(0, comma)};
        const std::string ys{text.substr(comma + 1)};
        std::size_t xn = 0;
        std::size_t yn = 0;
        const float x = std::stof(xs, &xn);
        const float y = std::stof(ys, &yn);
        if (xn != xs.size() || yn != ys.size())
            return std::nullopt;
        return pulp::view::Point{x, y};
    } catch (...) {
        return std::nullopt;
    }
}

// Key names accepted by --key: an optional "shift+" prefix, then a named key
// or a single lowercase letter or digit.
std::optional<std::pair<pulp::view::KeyCode, std::uint16_t>> parse_key(std::string_view text) {
    using pulp::view::KeyCode;
    std::uint16_t modifiers = 0;
    constexpr std::string_view shift_prefix = "shift+";
    if (text.substr(0, shift_prefix.size()) == shift_prefix) {
        modifiers = pulp::view::kModShift;
        text.remove_prefix(shift_prefix.size());
    }
    static const std::pair<std::string_view, KeyCode> named[] = {
        {"tab", KeyCode::tab},
        {"enter", KeyCode::enter},
        {"escape", KeyCode::escape},
        {"space", KeyCode::space},
        {"left", KeyCode::left},
        {"right", KeyCode::right},
        {"up", KeyCode::up},
        {"down", KeyCode::down},
        {"home", KeyCode::home},
        {"end", KeyCode::end_},
        {"page_up", KeyCode::page_up},
        {"page_down", KeyCode::page_down},
        {"backspace", KeyCode::backspace},
        {"delete", KeyCode::delete_},
    };
    for (const auto& [name, code] : named)
        if (text == name)
            return std::pair{code, modifiers};
    if (text.size() == 1 &&
        ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= '0' && text[0] <= '9')))
        return std::pair{static_cast<KeyCode>(text[0]), modifiers};
    return std::nullopt;
}

// The nearest DesignIR anchor at or above a view; hit-test results land on
// unanchored internals (a knob's label, a panel's background) as often as on
// the imported node itself.
std::string anchor_of(const pulp::view::View* view) {
    for (; view; view = view->parent())
        if (!view->anchor_id().empty())
            return view->anchor_id();
    return {};
}

// Child-index path from the root ("" for the root, "0/2" for the third child
// of the first child). Disambiguates views that share an anchor.
std::string path_of(const pulp::view::View* view) {
    std::string path;
    for (; view && view->parent(); view = view->parent()) {
        const auto* parent = view->parent();
        for (std::size_t i = 0; i < parent->child_count(); ++i) {
            if (parent->child_at(i) != view)
                continue;
            path = path.empty() ? std::to_string(i) : std::to_string(i) + "/" + path;
            break;
        }
    }
    return path;
}

pulp::view::View* find_focused(pulp::view::View& root) {
    if (root.has_focus())
        return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* found = find_focused(*root.child_at(i)))
            return found;
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
        } else if (auto* slider = dynamic_cast<const pulp::view::RangeSlider*>(&view)) {
            entry.addMember("kind", choc::value::createString("range_slider"));
            entry.addMember("value", choc::value::createFloat64(slider->value()));
        } else if (auto* toggle = dynamic_cast<const pulp::view::Toggle*>(&view)) {
            entry.addMember("kind", choc::value::createString("toggle"));
            entry.addMember("on", choc::value::createBool(toggle->is_on()));
        } else if (auto* button = dynamic_cast<const pulp::view::ToggleButton*>(&view)) {
            entry.addMember("kind", choc::value::createString("toggle_button"));
            entry.addMember("on", choc::value::createBool(button->is_on()));
        } else if (auto* box = dynamic_cast<const pulp::view::Checkbox*>(&view)) {
            entry.addMember("kind", choc::value::createString("checkbox"));
            entry.addMember("checked", choc::value::createBool(box->is_checked()));
        } else {
            stateful = false;
        }
        if (stateful)
            out.addArrayElement(entry);
    }
    for (std::size_t i = 0; i < view.child_count(); ++i)
        collect_state(*view.child_at(i), out);
}

choc::value::Value replay_step(pulp::view::View& root, const InteractionStep& step,
                               std::size_t index) {
    using Kind = InteractionStep::Kind;
    auto record = choc::value::createObject("");
    record.addMember("index", choc::value::createInt64(static_cast<std::int64_t>(index)));
    bool handled = false;
    const pulp::view::View* target_view = nullptr;
    if (step.kind == Kind::click || step.kind == Kind::drag) {
        record.addMember("kind",
                         choc::value::createString(step.kind == Kind::click ? "click" : "drag"));
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
            if (next)
                next->on_focus_changed(true);
            if (focused && focused != next)
                focused->on_focus_changed(false);
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
    record.addMember("target_path", choc::value::createString(target_view ? path_of(target_view)
                                                                          : std::string{"-"}));
    record.addMember("handled", choc::value::createBool(handled));
    const auto* focused_after = find_focused(root);
    record.addMember("focused", choc::value::createString(anchor_of(focused_after)));
    record.addMember(
        "focused_path",
        choc::value::createString(focused_after ? path_of(focused_after) : std::string{"-"}));
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

// How the render was produced. A parity lane compares renders from two
// implementations, so it has to know which rasterizer each side actually ran:
// Skia CPU and Skia Graphite on Dawn are different rasterizers, and a GPU
// request that quietly fell back to the CPU path would otherwise compare as a
// GPU result.
enum class Backend { skia, gpu };

// Development-only seeded faults for calibrating a GPU parity lane. Each one
// reproduces a failure the lane must never accept; none is reachable without
// the flag.
//   device-destroyed  destroy the Dawn device after the surface is created
//   no-frame          read back without drawing or submitting a frame
//   raster-fallback   render through Skia CPU and record that in the receipt
//   raster-fallback-unreported
//                     render through Skia CPU but record the GPU backend, the
//                     silent fallback a lane must catch from evidence other
//                     than the receipt (pixels, the GPU driver it never loaded)
enum class GpuFault {
    none,
    device_destroyed,
    no_frame,
    raster_fallback,
    raster_fallback_unreported
};

std::optional<GpuFault> parse_gpu_fault(std::string_view text) {
    if (text == "none")
        return GpuFault::none;
    if (text == "device-destroyed")
        return GpuFault::device_destroyed;
    if (text == "no-frame")
        return GpuFault::no_frame;
    if (text == "raster-fallback")
        return GpuFault::raster_fallback;
    if (text == "raster-fallback-unreported")
        return GpuFault::raster_fallback_unreported;
    return std::nullopt;
}

const char* gpu_fault_name(GpuFault fault) {
    switch (fault) {
    case GpuFault::none:
        return "none";
    case GpuFault::device_destroyed:
        return "device-destroyed";
    case GpuFault::no_frame:
        return "no-frame";
    case GpuFault::raster_fallback:
        return "raster-fallback";
    case GpuFault::raster_fallback_unreported:
        return "raster-fallback-unreported";
    }
    return "none";
}

struct RenderReport {
    std::string requested = "skia";
    std::string used;
    bool ok = false;
    std::string error;
    std::string fault = "none";
    // GPU only.
    bool surface_created = false;
    std::string frame_outcome;
    bool readback_completed = false;
    bool device_lost = false;
    std::uint32_t pixel_width = 0;
    std::uint32_t pixel_height = 0;
#if defined(PULP_HAS_SKIA)
    pulp::render::GpuSurface::AdapterInfo adapter;
#endif
};

#if defined(PULP_HAS_SKIA)
const char* adapter_type_name(pulp::render::GpuSurface::AdapterType type) {
    using T = pulp::render::GpuSurface::AdapterType;
    switch (type) {
    case T::integrated_gpu:
        return "integrated-gpu";
    case T::discrete_gpu:
        return "discrete-gpu";
    case T::cpu:
        return "cpu";
    case T::unknown:
        return "unknown";
    }
    return "unknown";
}

const char* frame_outcome_name(pulp::render::FrameOutcome outcome) {
    using F = pulp::render::FrameOutcome;
    switch (outcome) {
    case F::presented:
        return "presented";
    case F::offscreen:
        return "offscreen";
    case F::recreate:
        return "recreate";
    case F::failed:
        return "failed";
    }
    return "unknown";
}

// Renders through Skia Graphite on a Dawn device with no presentation surface:
// the composition HeadlessSurface uses, opened up so the receipt can record the
// adapter, the frame outcome and whether the device survived the frame. The
// paint sequence matches render_to_png_gpu (opaque clear, background fill,
// layout, paint, overlays), so a render here is comparable with one there.
std::vector<std::uint8_t> render_gpu(pulp::view::View& root, std::uint32_t width,
                                     std::uint32_t height, float scale, GpuFault fault,
                                     RenderReport& report) {
    auto gpu = pulp::render::GpuSurface::create_dawn();
    pulp::render::GpuSurface::Config gpu_config{};
    gpu_config.width = width;
    gpu_config.height = height;
    gpu_config.vsync = false;
    if (!gpu || !gpu->initialize(gpu_config)) {
        report.error = "no Dawn device (GpuSurface::initialize failed)";
        return {};
    }
    report.adapter = gpu->adapter_info();
    if (!report.adapter.available || report.adapter.null_backend) {
        // The Null backend accepts commands but reads back a flat frame; it is
        // never rendering evidence.
        report.error = "no native GPU adapter (Dawn Null backend)";
        return {};
    }
    auto skia = pulp::render::SkiaSurface::create(
        *gpu, {.width = width, .height = height, .scale_factor = scale});
    if (!skia || !skia->is_available()) {
        report.error = "SkiaSurface unavailable on this device";
        return {};
    }
    report.surface_created = true;
    if (fault == GpuFault::device_destroyed)
        pulp::render::render_observer::inject_device_loss(*gpu);

    if (fault != GpuFault::no_frame) {
        if (!gpu->begin_frame()) {
            report.error = "GpuSurface::begin_frame failed";
            report.device_lost = pulp::render::render_observer::device_lost(*gpu);
            return {};
        }
        auto* canvas = skia->begin_frame();
        if (!canvas) {
            gpu->end_frame();
            report.error = "SkiaSurface::begin_frame returned no canvas";
            report.device_lost = pulp::render::render_observer::device_lost(*gpu);
            return {};
        }
        canvas->set_fill_color(pulp::canvas::Color::rgba8(0, 0, 0, 255));
        canvas->fill_rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        canvas->set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
        canvas->fill_rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        root.set_bounds({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)});
        root.layout_children();
        {
            pulp::runtime::ScopedAllocAllowed offscreen_capture_is_not_realtime;
            root.paint_all(*canvas);
            pulp::view::View::paint_overlays(*canvas, &root);
        }
        report.frame_outcome = frame_outcome_name(skia->end_frame());
    } else {
        report.frame_outcome = "not-drawn";
    }

    pulp::render::HeadlessSurface::Rgba rgba;
    report.readback_completed = skia->read_current_rgba(rgba.pixels, rgba.width, rgba.height);
    gpu->end_frame();
    report.device_lost = pulp::render::render_observer::device_lost(*gpu);
    report.pixel_width = rgba.width;
    report.pixel_height = rgba.height;
    if (!report.readback_completed) {
        report.error = "GPU readback failed";
        return {};
    }
    if (report.device_lost) {
        report.error = "Dawn device was lost during the frame";
        return {};
    }
    std::string encode_error;
    auto png = pulp::render::HeadlessSurface::encode_png(rgba, &encode_error);
    if (png.empty())
        report.error = "PNG encode failed: " + encode_error;
    return png;
}
#endif

bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

std::string render_receipt_json(const RenderReport& report) {
    auto receipt = choc::value::createObject("");
    receipt.addMember("schema", choc::value::createString("pulp-design-ir-render-receipt-v1"));
    receipt.addMember("backend_requested", choc::value::createString(report.requested));
    receipt.addMember("backend_used", choc::value::createString(report.used));
    receipt.addMember("ok", choc::value::createBool(report.ok));
    receipt.addMember("error", choc::value::createString(report.error));
    receipt.addMember("seeded_fault", choc::value::createString(report.fault));
#if defined(PULP_HAS_SKIA)
    const auto versions = pulp::render::render_observer::toolchain_versions();
    receipt.addMember("skia_milestone", choc::value::createInt32(versions.skia_milestone));
    receipt.addMember("dawn_version", choc::value::createString(versions.dawn_version));
    if (report.requested == "gpu") {
        auto gpu = choc::value::createObject("");
        gpu.addMember("surface_created", choc::value::createBool(report.surface_created));
        gpu.addMember("frame_outcome", choc::value::createString(report.frame_outcome));
        gpu.addMember("readback_completed", choc::value::createBool(report.readback_completed));
        gpu.addMember("device_lost", choc::value::createBool(report.device_lost));
        gpu.addMember("pixel_width", choc::value::createInt64(report.pixel_width));
        gpu.addMember("pixel_height", choc::value::createInt64(report.pixel_height));
        auto adapter = choc::value::createObject("");
        adapter.addMember("available", choc::value::createBool(report.adapter.available));
        adapter.addMember("null_backend", choc::value::createBool(report.adapter.null_backend));
        adapter.addMember(
            "type", choc::value::createString(adapter_type_name(report.adapter.adapter_type)));
        adapter.addMember("backend", choc::value::createString(report.adapter.backend));
        adapter.addMember("backend_type", choc::value::createString(report.adapter.backend_type));
        adapter.addMember("name", choc::value::createString(report.adapter.name));
        adapter.addMember("vendor", choc::value::createString(report.adapter.vendor));
        adapter.addMember("architecture", choc::value::createString(report.adapter.architecture));
        gpu.addMember("adapter", adapter);
        receipt.addMember("gpu", gpu);
    }
#endif
    return choc::json::toString(receipt, true);
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
    std::filesystem::path render_receipt_path;
    Backend backend = Backend::skia;
    GpuFault gpu_fault = GpuFault::none;
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
        } else if (arg == "--click") {
            auto point = parse_point(value);
            if (!point) {
                std::cerr << "Error: --click expects <x>,<y>\n";
                return 2;
            }
            steps.push_back({.kind = InteractionStep::Kind::click, .start = *point});
        } else if (arg == "--drag") {
            const std::string_view text{value};
            const auto colon = text.find(':');
            auto from =
                colon == std::string_view::npos ? std::nullopt : parse_point(text.substr(0, colon));
            auto to = colon == std::string_view::npos ? std::nullopt
                                                      : parse_point(text.substr(colon + 1));
            if (!from || !to) {
                std::cerr << "Error: --drag expects <x1>,<y1>:<x2>,<y2>\n";
                return 2;
            }
            steps.push_back({.kind = InteractionStep::Kind::drag, .start = *from, .end = *to});
        } else if (arg == "--key") {
            auto key = parse_key(value);
            if (!key) {
                std::cerr << "Error: --key does not name a supported key: " << value << "\n";
                return 2;
            }
            steps.push_back({.kind = InteractionStep::Kind::key,
                             .key_name = value,
                             .key = key->first,
                             .modifiers = key->second});
        } else if (arg == "--interaction-log")
            interaction_log_path = value;
        else if (arg == "--render-receipt")
            render_receipt_path = value;
        else if (arg == "--backend") {
            const std::string_view name{value};
            if (name == "skia")
                backend = Backend::skia;
            else if (name == "gpu")
                backend = Backend::gpu;
            else {
                std::cerr << "Error: --backend expects skia or gpu\n";
                return 2;
            }
        } else if (arg == "--gpu-fault") {
            auto fault = parse_gpu_fault(value);
            if (!fault) {
                std::cerr << "Error: unknown --gpu-fault: " << value << "\n";
                return 2;
            }
            gpu_fault = *fault;
        } else {
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
    if (gpu_fault != GpuFault::none && backend != Backend::gpu) {
        std::cerr << "Error: --gpu-fault requires --backend gpu\n";
        return 2;
    }
    const auto serialized = read_text(input_path);
    if (serialized.empty()) {
        std::cerr << "Error: could not read DesignIR input\n";
        return 1;
    }
    pulp::view::DesignIR ir;
    try {
        ir = pulp::view::parse_design_ir_json(serialized);
    } catch (const pulp::view::DesignIrParseError& error) {
        std::cerr << "Error: invalid DesignIR: " << error.what() << "\n";
        return 1;
    }
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
        log.addMember("schema", choc::value::createString("pulp-design-ir-interaction-log-v1"));
        log.addMember("fixture", choc::value::createString(input_path.filename().string()));
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
    RenderReport report;
    report.requested = backend == Backend::gpu ? "gpu" : "skia";
    report.fault = gpu_fault_name(gpu_fault);
    const auto write_receipt = [&] {
        return render_receipt_path.empty() ||
               write_text(render_receipt_path, render_receipt_json(report));
    };
    const auto pixel_w = static_cast<std::uint32_t>(width);
    const auto pixel_h = static_cast<std::uint32_t>(height);
    const bool raster = backend == Backend::skia || gpu_fault == GpuFault::raster_fallback ||
                        gpu_fault == GpuFault::raster_fallback_unreported;
    std::vector<std::uint8_t> png;
    if (raster) {
        png = pulp::view::render_to_png(*root, pixel_w, pixel_h, scale,
                                        pulp::view::ScreenshotBackend::skia);
        report.used = gpu_fault == GpuFault::raster_fallback_unreported ? "gpu" : "skia";
        if (png.empty())
            report.error = "Skia raster render failed";
    } else {
#if defined(PULP_HAS_SKIA)
        png = render_gpu(*root, pixel_w, pixel_h, scale, gpu_fault, report);
        report.used = "gpu";
#else
        report.error = "this build has no GPU backend (PULP_HAS_SKIA is off)";
#endif
    }
    report.ok = !png.empty();
    if (!report.ok) {
        // Never substitute another backend: a missing GPU render is a
        // failure the caller has to see, not a CPU render under a GPU name.
        write_receipt();
        std::cerr << "Error: could not render DesignIR through " << report.requested << ": "
                  << report.error << "\n";
        return 1;
    }
    if (!write_bytes(render_path, png)) {
        std::cerr << "Error: could not write render\n";
        return 1;
    }
    if (!write_receipt()) {
        std::cerr << "Error: could not write render receipt\n";
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
