# Static tuning into a graph Processor

This installed-SDK example asks Pulp's public `TuningProvider` for MIDI note 69
at A4 = 442 Hz, validates the result off the audio callback, and gives that
frequency to one owned `Processor` in `SignalGraph`. The processor renders a
mono tone through the public offline graph host. It performs no tuning lookup,
file I/O, allocation, or graph mutation in `process()`.

Configure this directory against an installed Pulp SDK with
`cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<prefix>`, then build the
executable and run `ctest --test-dir build --output-on-failure`. The positive
oracle checks all 96 output samples against a 442 Hz sine at 48 kHz and checks
identical output with 32- and 17-frame graph blocks. An invalid MIDI note is
refused before graph construction. The planted `--planted-default-a4` run uses
440 Hz and must fail the same tuned audio oracle with exit 42.

This is a static, one-note, offline tuning recipe. It does not establish Scala
or MTS-ESP availability in the installed SDK, MIDI/MPE ingress, sample-timed
retuning, voice allocation, realtime safety, or graph save/load of Processor
nodes. Those remain separate G-11/G-5/G-6 work.
