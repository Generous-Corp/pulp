# PulpSampler

Sample-buffer sampler instrument with MIDI triggering, ADSR envelope, and pitch control.

## Formats
CLAP

## Features
- Off-thread mono/stereo sample-buffer publication
- MIDI note triggering with velocity sensitivity
- ADSR envelope per voice
- Pitch control via MIDI note number
- Multi-voice polyphony
- One-shot and loop playback

PulpSampler does not ship a file browser or audio-file decoder. Tests load
generated sample buffers through the processor helper API to validate sampler
playback, publication, and realtime-safety behavior.

## Source
`examples/PulpSampler/`
