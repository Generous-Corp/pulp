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
It also fills the tracker, applies same-sample CC120 or CC123, then starts a new
note; each channel-mode control must free the addressed physical-key records
before the capacity preflight decides whether the new note fits.
Polyphonic key pressure is refused before note or output mutation because this
bounded MIDI path does not route it; channel pressure remains supported.
The numeric first-sample oracle observes `sample3=-0.353553`, `sample10=0`.

`prove_mutations.py --sdk-prefix <installed-sdk> --scratch <build-directory>`
compiles independent mutations of the production binding source against the
installed SDK. It requires a positive, each planted failure, and restored
positive result, with per-case logs and a JSON receipt under the scratch path.

`a1_reference_vectors.json` freezes seven stereo cases for eight-slot gain,
sum order, sparse slots, bounded release, and hard steal. The separate
`verify_a1_reference.py` recomputes every expected sample with scalar f32
arithmetic and requires ten planted policy, routing, timing, gain, and tail
errors to fail. `generate_a1_vectors.py --check` binds the installed C++
consumer to the frozen JSON hash; the installed-SDK CTest runs both Python
checks before compiling the public consumer. The C++ consumer compares both
channels of every case at block sizes 1, 32, 64, 127, and 256.

The public consumer also renders an absolute-sample sustain trace at block
sizes 1, 2, 3, 7, and 16. It checks an audible held voice, release after the
last member/manager pedal rise, a four-frame tail, CC123 and CC121 audio
equivalence, and CC120 silence. A separate trace checks per-note expression
audio across those partitions. These are offline, in-process MIDI 1.0 and
Processor/SignalGraph checks on the tested macOS architecture. They do not
prove a DAW format adapter, AU/CLAP/VST3 hosted behavior, Linux/Windows
execution, callback deadlines, allocation/lock freedom under a paced host,
state/edit/undo, or arbitrary graph/polyphony configurations.

For this MIDI 1.0 profile, physical NoteOff removes a generation from the
tracker. Its sustain-held voice keeps the last member expression; later member
pitch/pressure/CC74 goes only to tracker-active generations, including a new
note that reuses the channel. Member CC121 explicitly resets the addressed
member's active, held, and releasing expression. Manager pitch/pressure/CC74
are typed refusals before audio or voice mutation: the underlying tracker
caches them without applying them to current voices, so zone-expression audio
needs a separate composition contract. Manager CC121 clears its pedal hold and
drains newly unheld NoteOff records; it does not claim zone-expression support.
