# G6 bounded voice-bank consumer

This executable uses only installed Pulp public headers and the `Pulp::host`
target. Its authored Processor borrows one of eight MPE allocator voices; one
prepared graph renders all slots. It compares fixed `1/sqrt(8)` gain, distinct
left/right samples, a triangle oscillator, and four-frame release against an
independent scalar oracle over the same absolute MIDI timeline at block sizes
1, 32, 64, 127, and 256. It also checks ordered eight-slot summation, audible
pitch/pressure/timbre changes, four ninth-note steal victims, hard steal audio
without an old-voice tail, and release reclamation. It refuses unknown note/expression
bindings, wrong authored paths, aliased slot state, stale deferred generations,
and disconnected or swapped stereo slot endpoints. The installed consumer also
fills all 128 distinct tracker keys, requires a typed atomic refusal for the
129th, releases one physical key, and proves a retry renders the new note.
Polyphonic key pressure is refused before note or output mutation because this
bounded MIDI path does not route it; channel pressure remains supported.
The numeric first-sample oracle observes `sample3=-0.353553`, `sample10=0`.

`prove_mutations.py --sdk-prefix <installed-sdk> --scratch <build-directory>`
compiles independent mutations of the production binding source against the
installed SDK. It requires a positive, each planted failure, and restored
positive result, with per-case logs and a JSON receipt under the scratch path.

The consumer covers the bounded note/audio path. Focused source tests cover
pedal and channel-mode behavior; installed pedal traces, state/edit/undo, and
paced realtime proof remain separate G6 requirements.

For this MIDI 1.0 profile, physical NoteOff removes a generation from the
tracker. Its sustain-held voice keeps the last member expression; later member
pitch/pressure/CC74 goes only to tracker-active generations, including a new
note that reuses the channel. Member CC121 explicitly resets the addressed
member's active, held, and releasing expression. Manager pitch/pressure/CC74
are typed refusals before audio or voice mutation: the underlying tracker
caches them without applying them to current voices, so zone-expression audio
needs a separate composition contract. Manager CC121 clears its pedal hold and
drains newly unheld NoteOff records; it does not claim zone-expression support.
