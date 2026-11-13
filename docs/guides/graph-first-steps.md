# First graph in five minutes

This page takes you from a fresh checkout to a validated, saved and measured
SignalGraph using only the `pulp graph` CLI and Python's standard library. Every
command below was run against a CLI built from this tree.

## Build the CLI

The `pulp` command you type is the Rust front end, which delegates graph
commands to the C++ CLI. Build both targets:

```sh
tools/ci/governed-build.sh cmake --build build --target pulp-cli pulp-rust-cli
```

`pulp-cli` produces `build/tools/cli/pulp-cpp`; `pulp-rust-cli` produces
`build/pulp`. Building only `pulp-cli` leaves you without the `pulp` front end.

## Write a patch

Save this as `first.pat`. The first line must be `# flow 1`. The default
catalog (`pulp graph catalog --json`) has audio in/out, `pulp/gain` and MIDI
in/out. The audio boundaries and `pulp/gain` are stereo, so their two channels
are the ports `audio` and `out1` (outputs) or `audio` and `in1` (inputs).

```text
# flow 1
input : pulp/audio_in
gain1 : pulp/gain
out : pulp/audio_out
input.audio >> gain1.audio
input.out1 >> gain1.in1
gain1.audio >> out.audio
gain1.out1 >> out.in1
gain1.gain = 0.5
```

Write each channel explicitly. A chain such as
`input.audio >> gain1.audio >> out.audio` is refused, because after the first
hop the chain continues from `gain1` with no port named, and a chain infers a
port only when the node has exactly one output. The refusal names the fix:

```text
pulp graph: line 5: ambiguous port: node 'gain1' has 2 output ports and a chain hop infers a port only when the node has exactly one output; write the port explicitly, e.g. gain1.audio >> next.audio; use one of: audio, out1
```

`gain1.gain = 0.5` sets the built-in gain node's setting. It is not an
automatable parameter, so `input.audio ~> gain1.gain @ 0.5` is refused; see the
[notation cookbook](graph-notation-cookbook.md).

## Validate and save

```sh
pulp graph validate first.pat
# valid: true
# format_version: 4
# nodes: 3
# connections: 4

pulp graph save first.pat --output first.pulpgraph
```

Add `--json` to `validate` for a machine-readable verdict (`"valid"`, `"code"`,
and on failure `"line"`, `"rule"`, `"suggested_fix"`).

## Make an input WAV

The probe reads a WAV file. This writes one second of a 440 Hz stereo sine at
48 kHz, 16-bit, using only the standard library:

```python
import math, struct, wave

with wave.open("in.wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(48000)
    frames = bytearray()
    for n in range(48000):
        v = int(0.5 * 32767 * math.sin(2 * math.pi * 440 * n / 48000))
        frames += struct.pack("<hh", v, v)
    w.writeframes(bytes(frames))
```

## Measure an edge with probe

`probe` runs the saved graph over the input and records peak and RMS on one
named audio edge, together with the execution plan and latency compensation:

```sh
pulp graph probe first.pulpgraph --input in.wav \
  --source-node gain1 --source-port audio \
  --destination-node out --destination-port audio \
  --plan --latency > probe.json
```

The JSON has `"valid": true`, `"code": "Attached"`, `"dropped": 0`, one record
per 64-frame block (750 for one second at 48 kHz; override with
`--block-frames`), and, because of the two flags, a `plan` object (nodes and
connections) and a `latency_pdc` object (per-node latency and per-connection
delay in samples; all 0 here). For the input above each record has a `peak` of
about 0.25: a 0.5 input through a 0.5 gain.

```python
import json
d = json.load(open("probe.json"))
print(d["code"], d["dropped"], max(r["peak"] for r in d["records"]))
print(d["latency_pdc"]["total_latency_samples"])
```

## Reading the float WAV that Pulp writes

`pulp graph render` writes 32-bit IEEE float WAV (format tag 3), which Python's
`wave` module rejects with `unknown format: 3`. Read it with `array`:

```python
import struct, sys
from array import array

def read_float_wav(path):
    data = open(path, "rb").read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        tag, size = struct.unpack_from("<4sI", data, pos)
        body = data[pos + 8 : pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif tag == b"data":
            samples = array("f")
            samples.frombytes(body[: size - size % 4])
            if sys.byteorder == "big":
                samples.byteswap()
        pos += 8 + size + (size & 1)
    assert fmt[0] in (3, 0xFFFE) and fmt[5] == 32, f"not 32-bit float: {fmt}"
    return fmt[1], fmt[2], samples  # channels, sample rate, interleaved floats

channels, rate, samples = read_float_wav("out.wav")
print(channels, rate, len(samples), max(abs(x) for x in samples))
```

## Known limitation: render takes no input audio

`pulp graph render` does not currently accept input audio. It ignores
`--input` without an error and renders the graph with silent input, so a graph
that processes `audio_in` writes an all-silent file. A fix is pending. Until it
lands, use `probe` (above) to measure a graph against a real input file, and do
not treat a `render` of an input-driven graph as evidence that it passes audio.

## Next

- [Notation cookbook](graph-notation-cookbook.md): what the default catalog
  can and cannot express, and what to register in C++ for the rest.
- [Patch notation reference](graph-patch-notation.md).
- [Observing an audio edge](graph-edge-probes.md).
