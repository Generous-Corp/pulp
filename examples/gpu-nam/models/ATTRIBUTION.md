# Bundled model attribution

`example.nam` is the example WaveNet capture from **Neural Amp Modeler Core**
(`sdatkinson/NeuralAmpModelerCore`, `example_models/wavenet.nam`), MIT-licensed.
It is included only as the GPU NAM example's default model so the demo runs
out-of-the-box; load your own `.nam` captures for real amp tones.

The `.nam` file format and the WaveNet architecture are the open, MIT-licensed
Neural Amp Modeler standard (https://github.com/sdatkinson/neural-amp-modeler,
https://github.com/sdatkinson/NeuralAmpModelerCore). Pulp's inference
(`examples/gpu-nam/nam_model.hpp`) and GPU forward (`GpuCompute::nam_forward`)
are independent implementations of that public architecture; this directory's
`example.nam` is the only third-party artifact, redistributed under its MIT
license with this attribution.
