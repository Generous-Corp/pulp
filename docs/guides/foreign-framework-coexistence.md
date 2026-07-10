# Foreign-framework coexistence (cooperative mode)

Pulp is built to **share a process with a second native UI framework** — a JUCE
plugin, a host application's own AppKit/Win32 UI, or another SDK — as long as
Pulp is embedded through its **plugin** host path. This page is the contract for
that "cooperative mode": what the other framework may and may not do, the init
order that keeps both alive, and the four platform risks that are easy to trip.

> This is the *forward* direction — a foreign framework hosting a Pulp view.
> For the *reverse* — a Pulp GPU UI living inside a JUCE plugin over your
> unchanged DSP — see [Putting a Pulp UI in a JUCE plugin](juce-embed.md).

## The one rule: `PluginViewHost`, never the standalone `WindowHost`

Pulp has two host surfaces and only one of them is a good process citizen:

| Host | Owns the run loop? | Mutates process-global state? | Use in a shared process |
|---|---|---|---|
| **`PluginViewHost`** (the plugin/embed path) | No — rides the host's loop | No | **Yes — this is cooperative mode** |
| **`WindowHost`** (the standalone app path) | **Yes — calls `[NSApp run]`** | **Yes** | **Never** |

The standalone `WindowHost::run_event_loop()` is deliberately hostile to
coexistence. On macOS it takes over the process:

- `[NSApp run]` — owns the main run loop and never returns until `[NSApp stop:]`
  (`window_host_mac.mm`).
- `[NSApp setActivationPolicy:…]` + `activateIgnoringOtherApps:` — claims the
  Dock icon and foreground activation.
- `install_app_menu(...)` → `[NSApp setMainMenu:…]` — replaces the app menu bar.

Those are correct for an app that *is* Pulp and fatal for one that only
*contains* Pulp. **Do not instantiate the standalone `WindowHost` in a process
another framework already drives.** Mount Pulp views through `PluginViewHost`
(what the VST3/AU/CLAP adapters and the embed SDK use), which installs no
run loop, no menu bar, and no activation policy — it attaches a child view to
the parent the host hands it and returns.

## Why `PluginViewHost` is already safe

The embed path is structurally cooperative by construction (verify against
`core/view/`, `core/events/`):

- **No global window registry.** `WindowManager` is not a singleton; instances
  are independent.
- **No app-level takeover.** No `[NSApp run]`, no `NSApplication` subclass, no
  `sendEvent:` swizzle, no app delegate, no activation-policy or menu-bar
  mutation from the plugin path.
- **No global input hooks.** No `addGlobalMonitorForEventsMatchingMask:` and no
  `SetWindowsHookEx` anywhere in the tree.
- **Per-instance frame clocks.** Each GPU editor gets its own `CVDisplayLink`;
  there is never a shared, process-wide frame clock to contend over.
- **A pump-agnostic event loop.** `pulp::events::EventLoop` / `Timer` are built
  on `std::thread` + `std::condition_variable`, *not* `CFRunLoopTimer` /
  `NSTimer` / `WM_TIMER`. They fire regardless of which framework owns the run
  loop, and regardless of the loop's current mode.
- **Main-thread work rides the host's loop.** Marshaled main-thread work uses
  `dispatch_get_main_queue()` (`plugin_main_thread_mac.mm`), so it runs on
  *whatever* main loop the host is already pumping — Pulp does not need its own.
- **Scoped key handling.** `performKeyEquivalent:` is scoped to the editor's own
  view tree and returns `false` for anything it does not consume
  (`plugin_view_host_mac.mm`), so host and foreign-framework shortcuts keep
  flowing.

## What the other framework may and may not do

**May, freely:**

- Own and pump the main run loop (that is the expected arrangement — Pulp rides
  it).
- Open, move, resize, focus, and close its own windows.
- Run modal loops (`runModal:`, `NSMenu` / event tracking) — see the timer note
  below.
- Run its own audio thread; Pulp's audio path is an independent real-time
  thread and does not touch the UI loop.

**Must not:**

- Instantiate Pulp's standalone `WindowHost` (owns `[NSApp run]` + menu +
  activation — see above).
- Assume Pulp installs a menu bar, an app delegate, or an activation policy —
  from the plugin path it installs none of these, and it must not, because the
  host owns them.
- Block the main queue indefinitely. Any Pulp main-thread work marshaled via
  `dispatch_get_main_queue()` only runs when the main queue is drained; a host
  that *never* returns to its loop starves it (see risk 3).

## Init order

1. The foreign framework initializes first and owns process-global setup
   (`NSApplication`, menu bar, activation policy on macOS; the main message loop
   on Windows).
2. Pulp attaches through `PluginViewHost` (adapter `createView` / embed
   `create`), which parents its child view under the container the host passes
   in. No global state is claimed.
3. Teardown reverses it: close the Pulp view (`bridge.close()` / adapter
   `removed()`), then let the host finish. Note that in VST3/CLAP the host
   often resets `plugin_view_host()` to `nullptr` *before* the close callback
   fires — do not dereference it in `on_view_closed`.

## The four risks to know

### 1. The standalone `WindowHost` is hostile — never in a shared process
Covered above. It owns `[NSApp run]`, the menu bar, and activation policy, and
calls `[NSApp stop:]`. It is the app entry point, not an embeddable view host.
Use `PluginViewHost`.

### 2. Windows plugin host has no self-clock
`plugin_view_host_win.cpp` is deliberately clock-less: there is **no internal
message loop**. Continuous animation depends on the foreign pump delivering
`WM_PAINT`, or on the embed caller driving frames explicitly via
`pulp_embed_tick`. Under a foreign **modal** pump that starves `WM_PAINT`,
animation stalls until the modal loop ends. This is by design — the host owns
the pump — but budget for it: drive `pulp_embed_tick` yourself if you need
frames during your own modal loops.

### 3. macOS: nested `[NSApp run]` / `runModal:` defers marshaled main-thread work
Pulp's `CVDisplayLink` ticks and `events::Timer` callbacks fire off the main
loop, but any work they **marshal to the main thread** (via
`dispatch_get_main_queue()`) only runs when the main queue is drained. A
*fully nested* foreign `[NSApp run]` or `runModal:` that never returns to drain
the queue defers that work until the modal loop exits. Off-thread work (the
timer's own callback, the audio thread) is unaffected — only main-queue-hop
work waits.

### 4. The standalone idle pump must use `kCFRunLoopCommonModes`
The CPU (CoreGraphics) window host drives its standalone repaint/screenshot
idle work off an `NSTimer`. A timer installed with
`+scheduledTimerWithTimeInterval:` lands in `NSDefaultRunLoopMode` **only**, so
it freezes the moment the run loop enters a modal or event-tracking mode — a
foreign framework's `runModal:`, or Pulp's own menu/context tracking. Pulp
registers this timer under `kCFRunLoopCommonModes` (via `NSRunLoopCommonModes`
in `window_host_mac.mm`) so it keeps ticking across those modes. The GPU host
is unaffected — its idle callback rides the `CVDisplayLink` display thread, not
the run loop.

> The sibling 60 fps animation timer in the CPU host uses the same
> `+scheduledTimerWithTimeInterval:` idiom and so shares the default-mode
> limitation; it has not been moved to common modes because no shipped path
> animates a CPU-host editor inside a foreign modal loop today. If that
> combination arises, apply the same fix *with a test that proves it* — the
> idle-pump change landed exactly that way (see below).

## What is covered by tests

`test/test_foreign_framework_coexistence.mm` spawns a **raw non-Pulp
`NSWindow`** (a faithful stand-in for a JUCE editor window) inside a Pulp
process and pins two invariants without ever running the hostile standalone
loop:

- The CPU-host idle pump keeps firing while the run loop is pumped **only** in
  `NSModalPanelRunLoopMode` and `NSEventTrackingRunLoopMode` — the regression
  test for risk 4. (With the old default-mode scheduling this assertion fails:
  the idle count freezes.)
- A `pulp::events::Timer` (its own thread), a background audio-render thread
  (models the host's audio callback), and Pulp UI hit-testing all stay correct
  across the foreign window's **open → resize → focus → close** lifecycle.

## Cannot be verified in CI

- Real DAW behavior with a genuinely foreign framework in-process (JUCE inside
  Logic/Live/Bitwig) — validate with a manual DAW smoke.
- Windows and Linux foreign-pump behavior (the risks above are documented from
  code; the automated test is macOS-only).

## See also

- [Putting a Pulp UI in a JUCE plugin](juce-embed.md) — the reverse direction.
- [Coming from JUCE](coming-from-juce.md) — migrating the DSP to a Pulp
  `Processor` instead of embedding.
