# Audio fixtures

`cross_platform_signal_chain.wav` is the byte-exact output of the fixed-input,
fixed-coefficient signal chain in `test_cross_platform_audio_golden.cpp`. Its
test target disables floating-point contraction so macOS, Linux, and Windows
must produce the same IEEE-754 samples and WAV bytes.

Regenerate deliberately with `PULP_REGENERATE_AUDIO_GOLDEN=1` after reviewing
an intentional DSP-contract change.
