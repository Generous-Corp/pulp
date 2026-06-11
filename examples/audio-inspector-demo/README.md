# Audio Inspector Demo

`pulp-audio-inspector-demo` is a small `StandaloneApp::run_with_editor()`
example that emits a continuous sine tone from `Processor::process()`.

It exists as the live GUI harness for the Audio Inspector:

```bash
./build/pulp run pulp-audio-inspector-demo --audio-probe-json /tmp/pulp-audio-inspector-demo.json --frames 90
./build/pulp run pulp-audio-inspector-demo --audio-inspector --screenshot /tmp/pulp-audio-inspector-demo.png --frames 90
```

The first command writes probe JSON with non-zero peak/RMS values. The second
command writes the main editor screenshot plus
`/tmp/pulp-audio-inspector-demo.audio-inspector.png`, proving the separate
inspector window can be captured from the real standalone host.

Use this example when you need a known-good target before debugging a real
plugin. A useful quick read is:

```bash
jq '{callbacks, peak_max, rms_max, peak_dbfs, rms_dbfs, clip_count, nan_inf_count}' \
  /tmp/pulp-audio-inspector-demo.json
```

Expected shape: callbacks advance, peak/RMS are non-zero, dBFS values are
finite, and clip/NaN counters stay at zero. If this demo is healthy but a real
target is silent, the problem is probably in that target's routing, input
stimulus, bypass state, graph wiring, or processor output path.

MCP clients should use the existing `pulp-mcp` tool:

```json
{
  "target": "pulp-audio-inspector-demo",
  "frames": 90
}
```

Reference fixtures captured from this harness live in `fixtures/`:

- `editor-main.png` - the auto-generated editor hosted through `run_with_editor()`
- `audio-inspector-panel.png` - the separate live Audio Inspector window with
  meters, waveform, and callback counters
