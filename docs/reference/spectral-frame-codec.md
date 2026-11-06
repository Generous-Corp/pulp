# Offline canonical spectral streams

`<pulp/signal/spectral_frame_codec.hpp>` provides an offline, header-only owned
value codec. `make_spectral_stream(geometry, count)` allocates zero bins and
contiguous sequence/origin records; `encode_spectral_stream(value)` returns owned
bytes; `decode_spectral_stream(bytes)` validates the complete stream before
allocating and returning a value. Results contain a named error, a memory receipt,
and an optional owned value. No failure publishes a partial value. Callers retain
their previous value until they explicitly replace it with a successful result.

These operations allocate and must run off the audio callback. They do not read
files, serialize Processors, route graph edges, or perform synthesis. An external
consumer can use the installed SDK target without a source checkout:

```cmake
find_package(Pulp CONFIG REQUIRED COMPONENTS signal)
add_executable(my_spectral_tool main.cpp)
target_link_libraries(my_spectral_tool PRIVATE Pulp::signal)
```

The standalone example in `examples/spectral-frame-codec-consumer` exercises this
installed-target path and includes a strict unknown-profile negative control.

## Values and conventions

Components are flat binary32 values in frame, channel, bin, real/imaginary order.
There are `2*C*(N/2+1)` components per frame. Frame records contain a u64 sequence
and signed i64 origin. Sequence starts at zero; origin starts at
`-floor((N-1)/H)*H` and advances by H. Completion is origin+N. The format caps
make every valid origin/completion representable; arbitrary crop positions and
publication epochs are not encoded.

Profile 1 fixes symmetric float Hann (`pulp.hann.symmetric.f32.v1`), forward
scale 1, inverse scale 1/N, local-window-origin phase, full-overlap startup,
and squared-window OLA normalization above binary32 1e-9. Layout 1 means ordered
indexed channels. Unknown profiles/layouts reject. N must be a power of two in
256..16384, H in 1..N/2, C in 1..16, and rate positive finite binary64.

Both endpoint imaginary components must be numerical zero. All components must
be finite; the encoder changes either zero sign to positive zero. The strict
decoder refuses negative zero. Subnormal and huge finite values retain their
bits. A codec-valid value is not evidence of synthesis safety.

## Wire format and identity

Integers and IEEE float bits use explicit little-endian encoding, without padding.

| Offset | Bytes | Header field |
| --- | --- | --- |
| 0 | 8 | `PULPSFRM` |
| 8 | 2 | version 1 |
| 10 | 2 | header size 64 |
| 12 | 4 | profile 1 |
| 16 | 4 | N |
| 20 | 4 | H |
| 24 | 2 | C |
| 26 | 2 | layout 1 |
| 28 | 4 | zero reserved |
| 32 | 8 | rate binary64 |
| 40 | 8 | frame count M |
| 48 | 8 | payload size |
| 56 | 8 | zero reserved |

Each record contains sequence u64, origin i64, then the components. Payload size
is exactly `M*(16+8*C*(N/2+1))`. Trailing bytes, truncation, reserved bits,
unknown versions, discontinuities and malformed numbers reject.

Content identity is lowercase SHA-256 of the entire canonical byte sequence.
It is external to the file, not authentication or source-asset identity. Applications
linking Pulp runtime can use `pulp::runtime::sha256_hex(bytes, size)` from
`<pulp/runtime/crypto.hpp>`; the codec itself needs no crypto/runtime dependency.

## Limits and ownership

M is 1..1048576, payload at most 64 MiB, and total input exactly 64+payload.
Empty streams reject. Each operation accepts a destination-memory budget,
default 128 MiB, reports its charge, and rejects an insufficient budget before
allocation. Owned arrays have exact sizes without vector capacity. Input storage
is reported separately. A conservative 4096-byte fixed scratch allowance is also
charged against the budget; validation allocates no temporary heap storage. General allocator bookkeeping is outside the byte charge.
Do not change the owning array sizes independently of their declared counts.

The file does not store original source length. A synthesis integration must
separately choose output extent and append `ceil(N/H)` zero synthetic frames on
the successive grid, including their windows in the OLA divisor. It must drain
incrementally and stop before last stored origin+N. This codec neither performs
that drain nor qualifies its tail output. It does not establish routed spectral
ports, analyzed assets, morph/modulation, or full spectral workflow completion.
