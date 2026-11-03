#pragma once

// The boundary between the slice's Pulp-side editor and its Vellum-side UI.
//
// Only standard C++ types cross it. The Vellum side (knob_panel.cpp) is built
// into its own shared library against the installed Vellum SDK and nothing
// from Pulp's tree; the Pulp side (vellum_editor.cpp) sees this header and
// nothing from Vellum. While Vellum's mirrored units still use `pulp::`
// namespaces, keeping the two in separate images is what stops one definition
// from binding to the other.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define SLICE_VELLUM_UI_API __attribute__((visibility("default")))
#else
#define SLICE_VELLUM_UI_API
#endif

namespace slice_vellum_ui {

/// What the panel calls back into its host. Values are normalized [0, 1].
struct PanelCallbacks {
    std::function<void()> begin;
    std::function<void(float)> set;
    std::function<void()> end;
};

class SLICE_VELLUM_UI_API KnobPanel {
  public:
    virtual ~KnobPanel();

    /// A knob bound to one normalized value, plus a meter.
    static std::unique_ptr<KnobPanel> create(std::uint32_t width, std::uint32_t height,
                                             float initial_value, PanelCallbacks callbacks);

    virtual void set_size(std::uint32_t width, std::uint32_t height, double scale) = 0;
    virtual std::uint32_t width() const = 0;
    virtual std::uint32_t height() const = 0;

    /// The host's value changed; move the knob without calling back.
    virtual void value_changed(float normalized) = 0;
    /// Latest meter level [0, 1].
    virtual void set_meter(float level) = 0;
    virtual void idle() = 0;

    /// The knob's current normalized value as the widget holds it.
    virtual float knob_value() const = 0;
    /// Vertical drag on the knob, in logical pixels from its centre.
    virtual void drag_knob(float dy_pixels) = 0;
    /// The current frame as premultiplied RGBA8, top row first, through
    /// Vellum's raster provider. False on failure.
    virtual bool render_rgba(std::vector<std::uint8_t>& rgba, std::uint32_t& pixel_width,
                             std::uint32_t& pixel_height) = 0;
    /// The current frame as PNG. Empty on failure.
    virtual std::vector<std::uint8_t> capture_png() = 0;
};

/// The number of live `KnobPanel`s, for leak checks across reopen.
SLICE_VELLUM_UI_API int live_panels();

} // namespace slice_vellum_ui
