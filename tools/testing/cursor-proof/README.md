# cursor-proof

Two instruments that answer "does the cursor change on hover, or only on
click?" — a question the two halves of Pulp's cursor pipeline can disagree on.

The pipeline has **two different resolvers**. The hover path
(`-mouseMoved:` / `-cursorUpdate:` → `resolveHoverCursorAt:`) publishes the
style of the deepest view under the pointer, *before* any handler runs. The
click path (`-mouseDown:` / drag delivery) publishes the captured drag
target's style *after* `deliver_mouse_down` handlers ran, and it publishes
unconditionally at every pointer phase. So a broken hover pass is masked: the
cursor still changes when you click, and only when you click. That is exactly
the shape of the bug these two tools exist to tell apart.

| | in-process | cross-process |
|---|---|---|
| | `test/test_mac_hover_cursor_live.mm` | `hover_cursor_probe.m` |
| target | a Catch2 fixture in a hidden window | any running app, by pid |
| oracle | `+[NSCursor currentCursor]` | `+[NSCursor currentSystemCursor]` |
| proves | the host publishes the right style | a person actually sees it change |

The in-process test is the gate: it runs in CI and fails if the host stops
publishing. The cross-process probe is the field instrument — point it at a
shipped build when someone reports "the cursor doesn't change" and it will say
whether hover is dead while drag still works.

## Building and running the probe

```sh
clang -fobjc-arc -framework AppKit -framework ApplicationServices \
    -o /tmp/hover_cursor_probe tools/testing/cursor-proof/hover_cursor_probe.m

/tmp/hover_cursor_probe --pid $(pgrep -x Spectr) --cols 12 --rows 8 --compare-drag
```

More than one distinct cursor over the sweep means hover feedback is live.
Exactly one means it is not. `--compare-drag` repeats the sweep with a button
held, which separates "no cursor code at all" from "feedback on drag only".

## Two things it does that are not optional

It **dissociates the pointer from the hardware mouse** so synthetic
positioning is authoritative — and wires re-association to every exit path
including fatal signals, because a process that dies mid-sweep otherwise
leaves the machine with a mouse that moves nothing until the user logs out.

It **voids a run it did not control**. A human touching the trackpad
overrides the synthetic position, which corrupts the verdict in both
directions: a missed sample reads as "never changed", and a real pointer
resting on a live region reads as a change the probe did not cause. Neither
is visible in the cursor value, so contention is detected positionally and
the run is discarded rather than averaged through.

## Why not a screenshot

macOS does not composite the cursor into `CGWindowListCreateImage` or
`screencapture` output, so a screenshot is not evidence about the cursor. The
probe hashes the cursor image bytes instead. Hashing rather than comparing
`NSCursor` pointers is what makes it work across processes: the singletons in
this process are not the ones the target set.
