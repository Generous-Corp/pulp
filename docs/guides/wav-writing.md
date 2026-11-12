# Writing WAV files

`write_wav_file()` in `pulp/audio/audio_file.hpp` writes 16-bit and 24-bit
integer and 32-bit float WAV (`WavBitDepth::Int16`, `Int24`, `Float32`), mono or
multichannel, from an `AudioFileData`.

## Atomic publication

A file is never written in place. The bytes go to a temporary file in the
target's directory (`.<name>.<random>.tmp`), the file is flushed to disk, and only
then renamed onto the target. The final path therefore holds either the complete
new file or whatever was there before:

- a write that fails or is cancelled part-way leaves no file at the target and no
  temporary file beside it;
- a previous file at the target survives a failed write unchanged;
- a reader never sees a truncated file.

`write_wav_file(path, data, depth)` replaces an existing file, as it always did.
Pass `WavReplace::FailIfExists` as a fourth argument to refuse instead: the call
returns false and the existing file is left as it was. On POSIX that check is a
`link()`, so two writers racing for one name cannot both succeed; on Windows it is
an existence check followed by a rename.

`write_file_atomically(path, replace, produce)` is the same publication step for
any writer: `produce` writes the whole file to the stream it is given and returns
false to abandon it (an error or a cancel). An exception from `produce` also
abandons the write.

## Header

The writer emits `WAVE_FORMAT_EXTENSIBLE` with the PCM or IEEE-float sub-format,
so a reader that checks only the base format tag sees `0xFFFE`; read the
sub-format GUID for the real format. For the integer depths, silence is exactly zero and every sample is within one
code of the exact value (`sample * 2^(bits-1)`).
Float32 is written bit-exactly, including values beyond ±1.0.

## Tests

`test/test_audio_file.cpp` (`[wav-atomic]`) checks the header fields and sample
bytes for each depth, a write that fails half-way (no target, no temporary file),
a failed write over an existing file, `FailIfExists`, and an unwritable
directory.
