"""The prompt a generator was started with, when it arrives by file.

The app does not put the prompt on the generator's command line: Linux refuses
any single argument over 128 KiB and macOS refuses argv plus the environment
over 1 MiB, so a long request failed to start at all. It writes the text to a
private file beside the run's log and passes `--prompt-file PATH` where the
prompt would have been. `resolve()` puts the text back in that place, so
everything after it reads argv exactly as it did when the prompt was an
argument.

The bytes are decoded with `os.fsdecode`, the decoding CPython applies to argv
on POSIX: UTF-8 with surrogateescape on macOS, and on Linux in a UTF-8 locale
or CPython's UTF-8 mode under the C/POSIX locale. The prompt is therefore the
same str it was as an argument for every byte string without a NUL. The one
exception is a Linux locale deliberately set to a non-UTF-8 codec, where argv
would have been decoded with that codec instead.
"""

from __future__ import annotations

import os

FLAG = "--prompt-file"


def resolve(argv: list[str], index: int) -> list[str]:
    """argv with `--prompt-file PATH` replaced by the file's text.

    `index` is where a positional prompt goes for this tool, and the flag must
    stand exactly there: anywhere else it would sit beside a prompt given
    positionally, and one of the two would be silently ignored. The file is
    removed once read; it existed only to carry the text across exec.
    """
    if FLAG not in argv:
        return list(argv)
    at = argv.index(FLAG)
    if at != index or at + 1 >= len(argv):
        raise SystemExit(f"{FLAG} PATH takes the place of the prompt "
                         f"(argument {index}), not an extra option")
    path = argv[at + 1]
    try:
        with open(path, "rb") as source:
            raw = source.read()
    except OSError as exc:
        raise SystemExit(f"cannot read the prompt file {path}: {exc}") from exc
    try:
        os.unlink(path)
    except OSError:
        pass
    if b"\0" in raw:
        raise SystemExit(f"the prompt file {path} contains a NUL byte")
    return list(argv[:at]) + [os.fsdecode(raw)] + list(argv[at + 2:])
