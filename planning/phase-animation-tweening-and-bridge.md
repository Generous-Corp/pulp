# Phase: Animation, Tweening, and JS Bridge Expansion

Date: 2026-03-26
Status: Ready for implementation
Branch: `feature/pulp-animation-tweening-and-bridge`
Worktree: `/Users/danielraffel/worktrees/pulp-animation-tweening-and-bridge`
Depends on: core/view (animation.hpp, view.hpp, widgets.hpp, widget_bridge.cpp)
Proposal: `planning/animation-tweening-and-bridge-proposal-2026-03-26.md`

---

## Goals

1. Make animation a first-class view concern with a shared clock and automatic invalidation
2. Add widget-local interaction animations to shipped primitives
3. Add motion tokens to the design token system
4. Expand the JS bridge to expose animation, events, and richer widget control
5. Test everything deterministically

---

## Milestone 1: AnimationClock + Invalidation

### 1A. `FrameClock` (new header)

**File:** `core/view/include/pulp/view/frame_clock.hpp`

A single authoritative time source for the view system. Not a wall-clock timer — it is advanced externally by whoever owns the run loop (native window host, SDL host, or test harness).

```cpp
namespace pulp::view {

class FrameClock {
public:
    // Advance the clock by dt seconds. Called once per frame by the host.
    void tick(float dt);

    // Current frame time (seconds since first tick)
    float time() const;

    // Delta since last tick
    float dt() const;

    // Frame counter (monotonic)
    uint64_t frame() const;

    // Subscribe to frame ticks. Returns subscription ID.
    // Callback receives dt. Subscriber is removed when it returns false.
    int subscribe(std::function<bool(float dt)> callback);
    void unsubscribe(int id);

    // Are any subscribers still active? (drives invalidation)
    bool has_active_subscribers() const;

    // Reset for testing
    void reset();
};

} // namespace pulp::view
```

Key design points:
- **Deterministic:** test code calls `clock.tick(0.016f)` to simulate 60fps frames
- **No threading:** FrameClock lives on the UI thread, period
- **Invalidation signal:** `has_active_subscribers()` tells the host whether to keep repainting

### 1B. Wire FrameClock into View

**File:** `core/view/include/pulp/view/view.hpp` (modify)

Add a FrameClock pointer to the View tree root. Views access it via `frame_clock()` which walks up to the root.

```cpp
// In View class:
void set_frame_clock(FrameClock* clock);
FrameClock* frame_clock() const;  // walks up parent chain
```

**File:** `core/view/src/view.cpp` (modify)

- Add `FrameClock* frame_clock_ = nullptr;` to View private members
- `set_frame_clock` sets it on the root
- `frame_clock()` returns own clock or parent's clock

### 1C. Wire FrameClock into AnimationManager

**File:** `core/view/include/pulp/view/animation.hpp` (modify)

AnimationManager gains an optional FrameClock binding. When bound, it auto-subscribes to the clock and ticks itself. When not bound (tests that call `tick()` manually), it works exactly as today.

```cpp
// In AnimationManager:
void bind(FrameClock& clock);   // subscribe to clock, auto-tick
void unbind();                  // unsubscribe
```

This is backward-compatible. Existing tests that call `mgr.tick(dt)` directly continue to work.

### 1D. `ValueAnimation<T>` — widget-local animation primitive

**File:** `core/view/include/pulp/view/animation.hpp` (add to existing)

A lightweight, embeddable value animator for widgets. No heap allocation. Designed to be a member variable of a widget.

```cpp
template<typename T = float>
class ValueAnimation {
public:
    ValueAnimation() = default;
    explicit ValueAnimation(T initial) : current_(initial), target_(initial) {}

    // Set a new target. Automatically starts animating.
    void animate_to(T target, float duration, EasingFunction ease = easing::ease_out_quad);

    // Snap to value immediately (no animation).
    void set(T value);

    // Advance by dt. Returns true if still animating.
    bool advance(float dt);

    // Current interpolated value.
    T value() const;

    // Is animation in progress?
    bool animating() const;

    // Cancel in-flight animation, keep current value.
    void cancel();
};
```

This replaces the pattern of widgets manually managing Tween instances. Usage in a widget:

```cpp
class Toggle : public View {
    ValueAnimation<float> thumb_position_{0.0f};  // 0=off, 1=on
    ValueAnimation<float> hover_opacity_{0.0f};

    void set_on(bool v) {
        on_ = v;
        thumb_position_.animate_to(v ? 1.0f : 0.0f, 0.15f, easing::ease_out_cubic);
    }
};
```

---

## Milestone 2: Motion Tokens

### 2A. Motion token definitions

**File:** `core/view/include/pulp/view/theme.hpp` (modify)

Add motion-related dimension tokens to the built-in themes:

| Token name | Dark default | Light default | Pro Audio default | Purpose |
|---|---|---|---|---|
| `motion.duration.fast` | 0.08 | 0.08 | 0.06 | Hover, focus ring |
| `motion.duration.normal` | 0.15 | 0.15 | 0.12 | Toggle, button press |
| `motion.duration.slow` | 0.30 | 0.30 | 0.25 | Panel open/close, tab switch |
| `motion.duration.meter_decay` | 0.30 | 0.30 | 0.30 | Meter RMS falloff |
| `motion.duration.peak_hold` | 1.50 | 1.50 | 1.50 | Peak indicator hold |

These are dimension tokens (floats), so they work with the existing `Theme::dimensions` map. No new types needed.

### 2B. Motion easing token (string tokens)

| Token name | Default | Purpose |
|---|---|---|
| `motion.easing.interaction` | `ease_out_cubic` | Default easing for UI interactions |
| `motion.easing.enter` | `ease_out_quad` | Element appearing |
| `motion.easing.exit` | `ease_in_quad` | Element disappearing |

These are string tokens. Widget code resolves them to `EasingFunction` pointers via a lookup helper:

```cpp
// In animation.hpp:
EasingFunction easing_by_name(const std::string& name);
```

### 2C. Add motion tokens to built-in themes

**File:** `core/view/src/theme.cpp` (modify)

Add the motion tokens to `Theme::dark()`, `Theme::light()`, and `Theme::pro_audio()`.

---

## Milestone 3: Widget Interaction Animations

Wire `ValueAnimation` into the shipped widget set. Each widget reads its motion durations from its resolved theme tokens. If no theme is available, use sensible hardcoded defaults.

### 3A. Toggle

- `thumb_position_`: animate between 0 (off) and 1 (on) over `motion.duration.normal`
- `hover_opacity_`: animate between 0 and 1 over `motion.duration.fast`
- Paint uses `thumb_position_.value()` to position the thumb and blend colors

### 3B. Knob

- `hover_glow_`: animate 0→1 on hover enter, 1→0 on hover leave, over `motion.duration.fast`
- `focus_ring_`: animate 0→1 on focus, over `motion.duration.fast`
- Paint reads glow/ring values for subtle highlight effects

### 3C. Fader

- `hover_thumb_scale_`: animate thumb width on hover
- Same timing as Knob

### 3D. Meter

- Already has `MeterBallistics` for RMS/peak smoothing
- Wire `motion.duration.meter_decay` and `motion.duration.peak_hold` tokens into the ballistics constants (currently hardcoded)

### 3E. Tooltip

- `opacity_`: animate 0→1 on show, 1→0 on hide, over `motion.duration.normal`
- Tooltip removes itself from tree when opacity reaches 0

### 3F. ComboBox

- `dropdown_opacity_`: animate 0→1 on open, 1→0 on close

### 3G. TabPanel

- `transition_progress_`: animate old→new tab content crossfade over `motion.duration.slow`

### Hover event requirement

Widgets need hover enter/leave events to drive hover animations. Currently View has `on_mouse_down`/`on_mouse_up`/`on_mouse_drag` but no hover.

**File:** `core/view/include/pulp/view/view.hpp` (modify)

```cpp
virtual void on_mouse_enter() {}
virtual void on_mouse_leave() {}
bool is_hovered() const { return hovered_; }
```

**File:** `core/view/src/view.hpp` (private member)

```cpp
bool hovered_ = false;
```

The host's event dispatch (or `simulate_hover()` for tests) sets `hovered_` and calls these.

---

## Milestone 4: JS Bridge Expansion

### 4A. Event callbacks (bridge → JS)

**File:** `core/view/src/widget_bridge.cpp` (modify)

Add bridge functions for richer event subscription:

```javascript
// JS API:
on(id, 'hover', function(entered) { ... });
on(id, 'click', function(x, y) { ... });
on(id, 'focus', function(gained) { ... });
on(id, 'select', function(index) { ... });   // already exists for ComboBox
```

Implementation: `wire_callbacks` expands to also register hover/click/focus listeners using the new View events.

### 4B. Animation control from JS

```javascript
// JS API:
animate(id, property, targetValue, durationMs, easing);
// Example:
animate('vol', 'value', 0.75, 300, 'ease_out_cubic');
animate('panel', 'opacity', 0, 200, 'ease_in_quad');

// Apply motion preset to a widget
setMotionPreset(id, 'snappy');   // uses fast durations
setMotionPreset(id, 'smooth');   // uses slow durations
setMotionPreset(id, 'none');     // disables animation
```

**File:** `core/view/src/widget_bridge.cpp` (modify)

`animate()` creates a `ValueAnimation` bound to the widget's property, subscribed to the root FrameClock.

`setMotionPreset()` overrides the widget's resolved motion tokens.

### 4C. Theme/token operations (already partially present)

The bridge already has `setTheme`, `applyTokenDiff`, `getThemeJson`. Expand with:

```javascript
// JS API:
setMotionToken(tokenName, value);     // e.g., setMotionToken('motion.duration.fast', 0.05)
getMotionToken(tokenName);            // returns current value
animateTokenDiff(json, durationMs);   // animated theme transition
```

### 4D. TabPanel and ListBox bridge functions

```javascript
createTabPanel(id, parentId);
addTab(panelId, title, contentId);     // contentId = a container already created
setActiveTab(panelId, index);

createListBox(id, parentId);
setListItems(id, ['item1', 'item2']);
getListSelection(id);
```

### 4E. Widget visibility and removal

```javascript
setVisible(id, bool);
removeWidget(id);          // remove from tree and bridge map
```

---

## Milestone 5: Testing

### 5A. FrameClock unit tests

**File:** `test/test_frame_clock.cpp` (new)

- tick advances time correctly
- subscribers called on tick
- subscriber returning false is removed
- `has_active_subscribers()` reflects actual state
- reset clears all state
- zero and negative dt handled safely

### 5B. ValueAnimation unit tests

**File:** `test/test_animation.cpp` (extend existing)

- animate_to reaches target
- set() snaps immediately
- cancel() stops mid-animation
- advance() returns false when finished
- chained animations (animate_to while already animating)
- zero duration snaps immediately

### 5C. Motion token tests

**File:** `test/test_theme.cpp` (extend or new section)

- built-in themes contain all motion tokens
- motion tokens parse from JSON
- easing_by_name resolves all named easings
- unknown easing name returns linear

### 5D. Widget animation behavior tests

**File:** `test/test_widget_animation.cpp` (new)

- Toggle: set_on(true) → advance → thumb_position reaches 1.0
- Toggle: hover enter → advance → hover_opacity reaches 1.0
- Knob: hover → advance → glow value changes
- Tooltip: show_at → advance → opacity reaches 1.0; hide → advance → opacity reaches 0.0
- All widgets: settled state matches expected end value after sufficient ticks

### 5E. Bridge animation tests

**File:** `test/test_widget_bridge.cpp` (extend existing)

- JS `animate()` call changes widget value over time
- JS `on(id, 'hover', ...)` fires callback
- JS `setMotionToken()` changes resolved duration
- JS `animateTokenDiff()` applies theme change smoothly

### 5F. Deterministic frame testing pattern

All animation tests use a manual FrameClock:

```cpp
FrameClock clock;
// ... bind widgets ...
clock.tick(0.016f);  // one frame at 60fps
clock.tick(0.016f);  // another frame
// assert intermediate state
for (int i = 0; i < 20; i++) clock.tick(0.016f);  // settle
// assert final state
```

No wall-clock timing. No sleeps. Fully deterministic.

---

## File Change Summary

### New files

| File | Purpose |
|---|---|
| `core/view/include/pulp/view/frame_clock.hpp` | FrameClock header |
| `core/view/src/frame_clock.cpp` | FrameClock implementation |
| `test/test_frame_clock.cpp` | FrameClock tests |
| `test/test_widget_animation.cpp` | Widget animation behavior tests |

### Modified files

| File | Changes |
|---|---|
| `core/view/include/pulp/view/animation.hpp` | Add `ValueAnimation<T>`, `easing_by_name()` |
| `core/view/include/pulp/view/view.hpp` | Add `frame_clock()`, hover events |
| `core/view/src/view.cpp` | Implement frame_clock accessor, hover tracking |
| `core/view/include/pulp/view/widgets.hpp` | Add `ValueAnimation` members to Knob, Fader, Toggle, Meter |
| `core/view/src/widgets.cpp` | Wire animations into paint/event handlers |
| `core/view/include/pulp/view/ui_components.hpp` | Add `ValueAnimation` to Tooltip, ComboBox, TabPanel |
| `core/view/src/ui_components.cpp` | Wire animations into paint/event handlers |
| `core/view/include/pulp/view/theme.hpp` | (no structural change, tokens added in .cpp) |
| `core/view/src/theme.cpp` | Add motion tokens to built-in themes |
| `core/view/src/widget_bridge.cpp` | New JS API functions for animation/events/tabs/list |
| `core/view/include/pulp/view/widget_bridge.hpp` | Add FrameClock* member |
| `core/view/CMakeLists.txt` | Add `src/frame_clock.cpp` |
| `test/CMakeLists.txt` | Add new test files |
| `test/test_animation.cpp` | Add ValueAnimation tests |

---

## Implementation Order

```
1. FrameClock                    [no dependencies]
2. ValueAnimation<T>             [no dependencies]
3. easing_by_name()              [no dependencies]
4. Motion tokens in themes       [no dependencies]
5. View::frame_clock()           [depends on 1]
6. View hover events             [no dependencies]
7. AnimationManager::bind()      [depends on 1]
8. Widget animations             [depends on 2, 4, 5, 6]
9. Bridge: event callbacks       [depends on 6]
10. Bridge: animate()            [depends on 1, 2, 5]
11. Bridge: motion tokens        [depends on 4]
12. Bridge: TabPanel/ListBox     [no dependencies]
13. Bridge: visibility/removal   [no dependencies]
14. All tests                    [incremental, per milestone]
```

Steps 1-4 can be done in parallel. Steps 5-7 can be done in parallel. Steps 8-13 can be done in parallel after their dependencies land.

---

## Acceptance Criteria

- [ ] `FrameClock` advances time deterministically in tests
- [ ] `ValueAnimation<T>` interpolates correctly with all easing functions
- [ ] All built-in themes include motion tokens
- [ ] Toggle visually animates thumb position on state change
- [ ] Knob shows hover glow when hovered
- [ ] Tooltip fades in/out instead of snapping
- [ ] JS `animate()` drives widget value changes over time
- [ ] JS `on(id, 'hover', fn)` fires on hover enter/leave
- [ ] All animation tests pass with deterministic frame stepping
- [ ] No wall-clock sleeps in any test
- [ ] All existing tests continue to pass (backward compatibility)
- [ ] Motion tokens are included in `getThemeJson()` output
- [ ] `applyTokenDiff()` can override motion tokens from JS

---

## What This Does NOT Include

- Spring/physics-based animation (future, Layer B)
- Timeline/choreography engine (future, Layer B)
- Visual regression screenshot tests (needs CI infrastructure)
- DAW compatibility testing (manual, pre-release)
- ScrollView animation/inertia (separate scope)
