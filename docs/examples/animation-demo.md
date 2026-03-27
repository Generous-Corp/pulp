# Animation Demo

This example shows how Pulp's animation system works with the JS bridge to create polished, interactive plugin UIs.

## JS UI Script

```javascript
// Create layout
createKnob('gain', 20, 20, 60, 60);
createKnob('mix', 100, 20, 60, 60);
createToggle('bypass', 180, 30, 50, 30);
createLabel('status', 'Ready', 20, 100, 200, 20);

// React to hover events
on('gain', 'hover', function(entered) {
    if (entered) {
        setValue('status_label', 'Adjusting gain...');
    }
});

on('bypass', 'toggle', function(value) {
    setValue('status_label', value ? 'Bypassed' : 'Active');
});

// Motion token customization — make it feel snappier
setMotionToken('motion.duration.fast', 0.05);
setMotionToken('motion.duration.normal', 0.10);
```

## What Animates Automatically

With the code above, you get these animations out of the box:

- **Knob hover glow**: a subtle accent-colored ring fades in when the cursor hovers over a knob
- **Toggle thumb slide**: the toggle thumb smoothly slides between off and on positions
- **Toggle hover highlight**: a subtle highlight appears on the toggle track on hover
- **Fader thumb scale**: the fader thumb grows slightly when hovered

All durations come from the theme's motion tokens and can be customized per-plugin or per-style-pack.

## C++ Equivalent

If you prefer C++ over JS:

```cpp
#include <pulp/view/widgets.hpp>
#include <pulp/view/frame_clock.hpp>

// In your editor component:
FrameClock clock_;
Knob gain_knob_;
Toggle bypass_toggle_;

void prepare() {
    root_view.set_frame_clock(&clock_);
    // Widgets automatically use motion tokens from the theme
}

void frame_tick(float dt) {
    clock_.tick(dt);
    // Widget animations advance via the clock
    // Knob glow, toggle thumb, etc. all animate smoothly
}
```

## Custom Animations

For animations beyond the built-in widget behaviors, use `ValueAnimation` directly:

```cpp
ValueAnimation panel_height_(0.0f);

void toggle_panel() {
    float target = panel_expanded_ ? 0.0f : 200.0f;
    float dur = resolve_dimension("motion.duration.slow", 0.3f);
    panel_height_.animate_to(target, dur, easing::ease_out_cubic);
    panel_expanded_ = !panel_expanded_;
}

void paint(canvas::Canvas& canvas) override {
    float h = panel_height_.value();
    if (h > 0.1f) {
        // Draw the expanding panel...
    }
}
```
