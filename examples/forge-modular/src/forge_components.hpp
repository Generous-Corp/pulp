#pragma once

// Forge Modular's own panel components.
//
// Rack's widget machinery is what we want and are entitled to: SvgKnob knows
// how to rotate to a value, SvgPort knows how a cable attaches, SvgSlider
// knows its travel. Its *graphics* are a different matter -- they are licensed
// CC BY-NC 4.0, and using them attaches a non-commercial condition to the
// artwork of every module anyone builds with Forge Modular, inherited without
// ever being told.
//
// So the machinery is subclassed and pointed at our own drawings. Every
// component is replaced, lights included: those load an SVG from the component
// library too rather than being drawn in code, which was easy to miss.
//
// The footprints match what the layout validator already enforces, so changing
// the art cannot quietly change what fits on a panel.

#include <rack.hpp>

// Declared here rather than including plugin.hpp, which includes this header:
// the generated placement code needs the components before the plugin's own
// model list exists.
extern rack::plugin::Plugin* pluginInstance;

namespace forge_modular {

inline std::shared_ptr<rack::window::Svg> art(const std::string& name) {
    return rack::window::Svg::load(
        rack::asset::plugin(pluginInstance, "res/components/" + name));
}

// ── knobs ───────────────────────────────────────────────────────────────────

struct ForgeKnob : rack::app::SvgKnob {
    ForgeKnob(const std::string& file) {
        // Rack's convention: a knob sweeps from -Ï€*0.83 to +Ï€*0.83, leaving
        // the gap at the bottom where the indicator would be unreadable.
        minAngle = -0.83f * M_PI;
        maxAngle = 0.83f * M_PI;
        setSvg(art(file));
    }
};

struct ForgeKnobLarge : ForgeKnob { ForgeKnobLarge() : ForgeKnob("knob-large.svg") {} };
struct ForgeKnobMedium : ForgeKnob { ForgeKnobMedium() : ForgeKnob("knob.svg") {} };
struct ForgeKnobSmall : ForgeKnob { ForgeKnobSmall() : ForgeKnob("knob-small.svg") {} };
struct ForgeTrimpot : ForgeKnob { ForgeTrimpot() : ForgeKnob("trimpot.svg") {} };

// ── jacks and screws ────────────────────────────────────────────────────────

struct ForgePort : rack::app::SvgPort {
    ForgePort() { setSvg(art("port.svg")); }
};

struct ForgeScrew : rack::app::SvgScrew {
    ForgeScrew() { setSvg(art("screw.svg")); }
};

// ── switches ────────────────────────────────────────────────────────────────

struct ForgeToggle : rack::app::SvgSwitch {
    ForgeToggle() {
        shadow->opacity = 0.f;
        addFrame(art("toggle-0.svg"));
        addFrame(art("toggle-1.svg"));
    }
};

struct ForgeSwitchThree : rack::app::SvgSwitch {
    ForgeSwitchThree() {
        shadow->opacity = 0.f;
        addFrame(art("switch3-0.svg"));
        addFrame(art("switch3-1.svg"));
        addFrame(art("switch3-2.svg"));
    }
};

struct ForgeButton : rack::app::SvgSwitch {
    ForgeButton() {
        momentary = true;
        shadow->opacity = 0.f;
        addFrame(art("toggle-0.svg"));
        addFrame(art("toggle-1.svg"));
    }
};

// ── sliders ─────────────────────────────────────────────────────────────────

struct ForgeSlider : rack::app::SvgSlider {
    ForgeSlider() {
        setBackgroundSvg(art("slider-bg.svg"));
        setHandleSvg(art("slider-handle.svg"));
        // Travel is the background's height less the handle, so the thumb
        // stops flush with each end instead of overhanging it.
        maxHandlePos = rack::math::Vec(0.f, 0.f);
        minHandlePos = rack::math::Vec(
            0.f, background->box.size.y - handle->box.size.y);
    }
};

// ── lights ──────────────────────────────────────────────────────────────────

template <typename TBase>
struct ForgeLight : rack::componentlibrary::TSvgLight<TBase> {
    explicit ForgeLight(const std::string& file) { this->setSvg(art(file)); }
};

template <typename TBase>
struct ForgeTinyLight : ForgeLight<TBase> {
    ForgeTinyLight() : ForgeLight<TBase>("light-tiny.svg") {}
};
template <typename TBase>
struct ForgeSmallLight : ForgeLight<TBase> {
    ForgeSmallLight() : ForgeLight<TBase>("light-small.svg") {}
};
template <typename TBase>
struct ForgeMediumLight : ForgeLight<TBase> {
    ForgeMediumLight() : ForgeLight<TBase>("light-medium.svg") {}
};
template <typename TBase>
struct ForgeLargeLight : ForgeLight<TBase> {
    ForgeLargeLight() : ForgeLight<TBase>("light-large.svg") {}
};

}  // namespace forge_modular
