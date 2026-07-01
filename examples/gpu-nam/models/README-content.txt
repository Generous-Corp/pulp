GPU NAM — sample content
=========================

This folder holds sample Neural Amp Modeler content the GPU NAM installer
placed here so the plugin has something to load out of the box. You can keep,
copy, move, or delete anything here — the plugin only reads it when you pick a
file, and it also looks here first when you open its "Load model" / "Load
cabinet" choosers.

Models/            NAM captures (.nam) — pick one with the plugin's model chooser
  wavenet.nam                  Small WaveNet capture (3 channels). GPU-eligible.
  wavenet_a1_standard.nam      Full "standard" WaveNet capture (16 channels),
                               the size a real amp capture typically uses.
                               GPU-eligible.
  lstm.nam                     LSTM capture. Runs on the exact CPU engine
                               (the GPU forward path is WaveNet-only).

Cabinets/          Impulse responses (.wav) — pick one with the IR chooser
  cabinet.wav                  A short synthetic guitar-cabinet impulse response
                               to color the amp model. Swap in your own IRs.

These .nam models are the open, MIT-licensed Neural Amp Modeler example
captures (github.com/sdatkinson/NeuralAmpModelerCore, example_models). The
cabinet IR is a synthetic impulse. See the plugin's model
attribution for details. Load your own .nam captures and IRs for real tones.

GPU NAM's file choosers default to this folder. To point them somewhere else,
set the GPU_NAM_CONTENT environment variable to a directory of your choosing;
it also honors ~/Music/GPU NAM and ~/Documents/GPU NAM if either exists.
