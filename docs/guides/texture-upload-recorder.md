# Texture upload recorder

The texture upload recorder lists every texture an editor uploads to the GPU,
with its size, pixel format and bytes. Two editors that should draw the same
skin can then be compared upload by upload, to prove their decoded textures are
byte-identical.

It is test-only tooling. It lives in `tools/testing/texture_upload_recorder`,
is not installed or exported with the SDK, and must never be linked into a
shipped plug-in or runtime library. It records nothing until a test or test host
calls `install()`.

Supported today: Metal on macOS.

## Recording

```cpp
#include <pulp/testing/texture_upload_recorder.hpp>

pulp::testing::TextureUploadRecorder::Options options;
options.output_dir = "/path/outside/any/repository/run-a";
std::string error;
if (!pulp::testing::TextureUploadRecorder::install(options, &error))
    fail(error);
// ... load the plug-in, open its editor, let it draw ...
pulp::testing::TextureUploadRecorder::stop();
```

Link the test target to `pulp::texture-upload-recorder`. `install()` wraps the
Metal calls that upload texture data, in this process only:

- `MTLDevice newTextureWithDescriptor:`, to number textures,
- `MTLTexture replaceRegion:` in both forms,
- `MTLBlitCommandEncoder copyFromBuffer:...toTexture:` in both forms, which is
  the staging-buffer path. The bytes are read from the buffer's CPU contents
  when the copy is encoded. A buffer without CPU contents (private storage) is
  recorded with its geometry and an empty hash.

Each wrapped call runs the original implementation with the original arguments,
so recording does not change what the editor draws.

## Output

`output_dir/manifest.jsonl` holds one line per upload: `seq`, `api`, `entry`,
`texture_id`, the region (`x`, `y`, `z`, `width`, `height`, `depth`), `mip`,
`slice`, `pixel_format`, `source_bytes_per_row`, `padding_stripped`,
`byte_length`, `sha256`, `raw_file` and `caller_image`, the binary that made
the call.

Unless `write_raw` is off, each upload's bytes are also written to `raw_file`.
Hashes and raw files cover the pixels with the source's row padding removed, so
the same pixels uploaded with a different stride hash the same. For a pixel
format whose size the recorder does not know, the rows are kept as given and
`padding_stripped` is false.

If a manifest line cannot be written (a full disk, say), recording stops,
`manifest.jsonl` is removed and `TextureUploadRecorder::failure()` says why, so
an incomplete run can never be compared as if it were whole. Check `failure()`
after `stop()`.

Raw textures recorded from a real product are that product's decoded art.
`install()` refuses an `output_dir` inside a git working tree unless
`allow_inside_repository` is set, so recordings do not end up committed by
accident.

## Comparing two runs

```bash
python3 tools/testing/texture_upload_recorder/texture_upload_compare.py run-a run-b
```

The comparison treats each run as a multiset of (width, height, depth, pixel
format, sha256), because two editors may upload in a different order. It reports
how many uploads matched and lists the ones found in only one run, by hash, size
and format; it never prints pixels. It exits 0 when every upload matches and 1
otherwise, and 2 when a run is missing its manifest or holds a malformed line
(a size that is not a non-negative integer, say). An upload whose bytes could
not be read never counts as a match.
`--json` prints the same result as JSON.
