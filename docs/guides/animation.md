# Animation Guide

Pulp provides a first-class animation system integrated into the view layer. Animations are deterministic, theme-aware, and work seamlessly with both C++ widgets and the JS bridge.

## Core Concepts

### FrameClock

The `FrameClock` is the single authoritative time source for all view animations. It lives on the UI thread and is advanced once per frame by the window host.

```cpp
#include <pulp/view/frame_clock.hpp>

FrameClock clock;
root_view.set_frame_clock(&clock);

// In your render loop:
clock.tick(dt_seconds); // e.g., 0.016f for 60fps
```

Children access the clock via `frame_clock()`, which walks up the parent chain.

### ValueAnimation

`ValueAnimation` is a lightweight, embeddable animator designed to be a member variable of a widget. No heap allocation.

```cpp
#include <pulp/view/animation.hpp>

ValueAnimation opacity(0.0f);       // start at 0
opacity.animate_to(1.0f, 0.15f);    // fade in over 150ms
opacity.animate_to(1.0f, 0.15f, easing::ease_out_cubic); // with easing

// In your frame tick:
opacity.advance(dt);
float current = opacity.value();     // interpolated value
bool still_going = opacity.animating();
```

### Easing Functions

Pulp includes these easing functions in `pulp::view::easing`:

| Function | Character |
|---|---|
| `linear` | Constant speed |
| `ease_in_quad` | Slow start |
| `ease_out_quad` | Slow end |
| `ease_in_out_quad` | Slow start and end |
| `ease_in_cubic` | Slower start |
| `ease_out_cubic` | Slower end (good default) |
| `ease_in_out_cubic` | Smooth S-curve |
| `ease_in_expo` | Exponential acceleration |
| `ease_out_expo` | Exponential deceleration |
| `ease_out_elastic` | Springy overshoot |
| `ease_out_bounce` | Bouncing settle |

Resolve by name with `easing_by_name("ease_out_cubic")`.

## Motion Tokens

Animation durations and easing choices are part of the design token system, not hardcoded constants. This means the AI Style Designer can change the "feel" of a plugin, not just its colors.

### Duration Tokens

| Token | Dark default | Pro Audio | Purpose |
|---|---|---|---|
| `motion.duration.fast` | 0.08s | 0.06s | Hover, focus ring |
| `motion.duration.normal` | 0.15s | 0.12s | Toggle, button press |
| `motion.duration.slow` | 0.30s | 0.25s | Panel open/close |
| `motion.duration.meter_decay` | 0.30s | 0.30s | Meter RMS falloff |
| `motion.duration.peak_hold` | 1.50s | 1.50s | Peak indicator hold |

### Easing Tokens

| Token | Default |
|---|---|
| `motion.easing.interaction` | `ease_out_cubic` |
| `motion.easing.enter` | `ease_out_quad` |
| `motion.easing.exit` | `ease_in_quad` |

### Using Tokens in Widgets

```cpp
void MyWidget::on_mouse_enter() {
    float dur = resolve_dimension("motion.duration.fast", 0.08f);
    hover_.animate_to(1.0f, dur, easing::ease_out_quad);
}
```

## Built-in Widget Animations

These animations are built into Pulp's shipped widgets:

| Widget | Animation | Token |
|---|---|---|
| Toggle | Thumb slides between on/off | `motion.duration.normal` |
| Toggle | Hover highlight | `motion.duration.fast` |
| Knob | Hover glow ring | `motion.duration.fast` |
| Fader | Thumb scale on hover | `motion.duration.fast` |
| Tooltip | Fade in/out | `motion.duration.normal` |

## JS Bridge Animation API

### animate()

```javascript
animate(widgetId, property, targetValue, durationMs, easingName);
// Example:
animate('volume', 'value', 0.75, 300, 'ease_out_cubic');
```

### Motion Token Control

```javascript
setMotionToken('motion.duration.fast', 0.05);  // make hover snappier
getMotionToken('motion.duration.fast');          // read current value
```

### Widget Visibility

```javascript
setVisible('panel', false);   // hide
setVisible('panel', true);    // show
removeWidget('panel');        // remove from tree entirely
```

## Testing Animations

All animation tests use deterministic frame stepping -- no wall-clock timing:

```cpp
FrameClock clock;
Toggle toggle;
toggle.set_on(true);

// Step through frames
for (int i = 0; i < 20; i++)
    toggle.advance_animations(0.016f);

// Assert settled state
REQUIRE(toggle.thumb_position() == Approx(1.0f));
```

## Design Philosophy

- Animation is **subtle by default**. The goal is polished, not flashy.
- Motion timing belongs in the **design system**, not scattered constants.
- Widget-local animations are **cheap and ubiquitous** -- they're member variables, not managed objects.
- The FrameClock is **deterministic** -- tests never use wall-clock sleeps.
