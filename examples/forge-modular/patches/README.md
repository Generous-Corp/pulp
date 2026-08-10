# Test patches

Open with **File → Open** in VCV Rack, or from a terminal:

```sh
"/Applications/VCV Rack 2 Free.app/Contents/MacOS/Rack" <patch>.vcv
```

| Patch | What it shows |
|---|---|
| `all-modules.vcv` | All ten modules side by side, unpatched. Look at the panels. Makes no sound. |
| `voice.vcv` | The complete voice, wired and **playing** — LFO clocks EUCLID and SEQ, EUCLID gates ENV, SEQ drives VCO pitch, VCO → VCF → VCA → audio out + scope. **This makes sound**: pick an output device in the AUDIO module first. |
| `knob-test.vcv` | Our VCO beside Fundamental's. Drag the FREQ knob on **both**. If both slam to maximum, the knob bug is Rack's cursor-warp workaround, not ours. If only ours does, it is ours and needs reopening. |

## If a knob slams to maximum

Known Rack issue on macOS 26 — Rack warps the cursor every frame during a drag
(`src/window/Window.cpp:650`, citing glfw#2523) and derives the value from the
resulting deltas. Mitigation:

```sh
# set "allowCursorLock": false in Rack's settings
python3 - <<'PY'
import json, os
p = os.path.expanduser('~/Library/Application Support/Rack2/settings.json')
s = json.load(open(p)); s['allowCursorLock'] = False
json.dump(s, open(p, 'w'), indent=2)
PY
```

## If Rack opens to a "recover from last session?" dialog

Rack's log was truncated by a hard kill, and that modal silently swallows any
patch passed on the command line. Delete `~/Library/Application Support/Rack2/log.txt`
and relaunch.
