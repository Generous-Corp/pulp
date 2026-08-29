These fixtures pin the on-disk format written by VCV Rack 2 when it saves a
patch: a Zstandard-compressed tar with a root `patch.json` member.

`rack-saved-valid.vcv` contains `source/valid/patch.json`,
`rack-saved-malformed.vcv` contains `source/malformed/patch.json`, and
`rack-saved-missing.vcv` contains only `source/missing/readme.txt`. They were
created on macOS through `/usr/bin/tar` with a local zstd encoder available:

```sh
COPYFILE_DISABLE=1 /usr/bin/tar --no-xattrs --zstd \
  -cf rack-saved-valid.vcv -C source/valid patch.json
```

The malformed and missing variants use the same command with their respective
source directory and member. Keeping the tiny sources beside the binary files
makes the archive fixtures reproducible and reviewable.

`rack-saved-rack2-valid.vcv` matches Rack 2.6.6's exact member layout: a
zero-byte `./` directory marker followed by regular `./patch.json`. The simpler
root-member fixture remains as compatibility coverage.

The `traversal`, `absolute`, `symlink`, `hardlink`, `nonregular`, `oversized`,
`duplicate`, `checksum`, `trailing`, `root-directory-payload`, and
`nested-directory` variants are adversarial tar archives.
They prove the decoder rejects unsafe paths, non-regular members, expansion
beyond the declared limit, duplicate root `patch.json` members, invalid header
checksums, and trailing ambiguity without extracting anything. They were
assembled with Python's `tarfile` writer and compressed with zstd v1.5.7; their
committed SHA-256 values are asserted by source review and their exact rejection
reasons are asserted by `test_generate_safety.py`.
