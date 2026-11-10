# G6 bounded voice-bank consumer

This executable uses only installed Pulp public headers and the `Pulp::host`
target. Its authored Processor borrows one of eight MPE allocator voices; one
prepared graph renders all slots. It compares a fixed triangle oscillator and
four-frame release against an independent scalar oracle over the same absolute
MIDI timeline at block sizes 1, 32, 64, 127, and 256, then checks four ninth-note
steal victims and release reclamation. It refuses unknown note/expression
bindings, wrong authored paths, aliased slot state, stale deferred generations,
and disconnected or swapped stereo slot endpoints. The numeric first-sample
oracle observes `sample3=-0.125`, `sample6=-0.11125`, and `sample10=0`.

`prove_mutations.py --sdk-prefix <installed-sdk> --scratch <build-directory>`
compiles independent mutations of the production binding source against the
installed SDK. It requires a positive, each planted failure, and restored
positive result, with per-case logs and a JSON receipt under the scratch path.

The consumer covers the bounded note/audio path. Pedals, full state/edit/undo,
paced realtime proof remain separate G6 requirements.
