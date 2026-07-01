# Bundled model attribution

The `.nam` captures in this directory are the open, **MIT-licensed** Neural Amp
Modeler example models from **NeuralAmpModelerCore**
(`sdatkinson/NeuralAmpModelerCore`, `example_models/`), redistributed under
their MIT license with this attribution. They are included only so GPU NAM runs
out of the box and so you can test both engines; load your own `.nam` captures
for real amp tones.

| File | Source (NeuralAmpModelerCore/example_models) | Architecture |
|------|----------------------------------------------|--------------|
| `example.nam` | `wavenet.nam` | WaveNet (A1), 3 channels — the demo's default model |
| `wavenet_a1_standard.nam` | `wavenet_a1_standard.nam` | WaveNet (A1-Standard), 16 channels |
| `lstm.nam` | `lstm.nam` | LSTM |

`cabinet.wav` is a short **synthetic, original** guitar-cabinet impulse
response generated for this example — not a third-party capture.

`README-content.txt` is the note the installer places alongside the samples it
installs to `/Library/Application Support/GPU NAM/`.

The `.nam` file format and the WaveNet / LSTM architectures are the open,
MIT-licensed Neural Amp Modeler standard
(https://github.com/sdatkinson/neural-amp-modeler,
https://github.com/sdatkinson/NeuralAmpModelerCore). Pulp's inference
(`examples/gpu-nam/nam_model.hpp`, `nam_lstm.hpp`) and GPU forward
(`GpuCompute::nam_forward`) are independent implementations of those
public architectures; these `.nam` files are the only third-party artifacts.

## A note on NAM Architecture 2 (A2)

These are **A1** (original WaveNet) and LSTM captures. GPU NAM's engine
implements the standard A1 WaveNet and LSTM architectures only. NAM's newer
**A2** architecture (LeakyReLU activations, a convolutional head, mixed kernel
sizes per layer array, grouped convolutions, and packed-slimmable Full/Lite
sizing) is a distinct architecture GPU NAM does not yet model — such files are
rejected cleanly at load rather than mis-rendered. To source loadable captures
online, use TONE3000's "A1 – Legacy" architecture filter.
