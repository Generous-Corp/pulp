# Zstandard single-file decompressor

`zstddeclib.c` is the official decompression-only amalgamation generated from
Meta's Zstandard v1.5.7 source release. It is used only by
`rack_patch_decode.cpp`; no compressor or command-line program is included.

- Upstream: <https://github.com/facebook/zstd>
- Release: `v1.5.7`
- Release archive SHA-256:
  `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`
- Generation command, from `zstd-1.5.7/build/single_file_libs`:

  ```sh
  ./combine.sh -r ../../lib -x legacy/zstd_legacy.h \
    -o zstddeclib.c zstddeclib-in.c
  ```

- Generated `zstddeclib.c` SHA-256:
  `e1dc239cb4bcf3c00a0462fdbe5f3e3c78f907c4f8cb02c048807d754bdc51b5`
- Included upstream `LICENSE` SHA-256:
  `7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8`

The upstream file is dual-licensed; Pulp uses the BSD-style license reproduced
in `LICENSE`.
