// TextureUploadRecorder on Metal: three known uploads (replaceRegion, the
// sliced replaceRegion with padded rows, and a staging-buffer blit) are recorded
// exactly, byte for byte, without changing what reaches the textures. A second
// run with one byte changed in one texture records a different sha256 for that
// texture only. Hooked classes that inherit or call super reach the original
// exactly once, and a failed manifest write stops recording and removes the
// manifest.
#include <pulp/testing/texture_upload_recorder.hpp>

#include "recorder_core.hpp"

#include "support/unique_temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#import <CommonCrypto/CommonDigest.h>
#import <Metal/Metal.h>

#include <sys/resource.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Stand-ins for a texture class hierarchy: a base that uploads, a subclass that
// inherits the upload, and subclasses that override it and call super.
// Each class counts its own entries; an override entered again while it is
// still running would mean the dispatch re-entered it instead of reaching super.
namespace {
int g_base_calls = 0;
int g_override_calls = 0;
int g_override_depth = 0;
bool g_override_reentered = false;

void enter_override() {
    ++g_override_calls;
    if (++g_override_depth > 1)
        g_override_reentered = true;
}
} // namespace

@interface PulpRecorderFakeTexture : NSObject
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
            withBytes:(const void*)bytes
          bytesPerRow:(NSUInteger)bytesPerRow;
@end

@implementation PulpRecorderFakeTexture
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
            withBytes:(const void*)bytes
          bytesPerRow:(NSUInteger)bytesPerRow {
    ++g_base_calls;
}
@end

@interface PulpRecorderFakeTextureInherits : PulpRecorderFakeTexture
@end
@implementation PulpRecorderFakeTextureInherits
@end

@interface PulpRecorderFakeTextureCallsSuper : PulpRecorderFakeTexture
@end
@implementation PulpRecorderFakeTextureCallsSuper
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
            withBytes:(const void*)bytes
          bytesPerRow:(NSUInteger)bytesPerRow {
    enter_override();
    if (g_override_depth == 1)
        [super replaceRegion:region mipmapLevel:level withBytes:bytes bytesPerRow:bytesPerRow];
    --g_override_depth;
}
@end

// The same shape again, hooked subclass first.
@interface PulpRecorderFakeTexture2 : NSObject
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
            withBytes:(const void*)bytes
          bytesPerRow:(NSUInteger)bytesPerRow;
@end

@implementation PulpRecorderFakeTexture2
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
            withBytes:(const void*)bytes
          bytesPerRow:(NSUInteger)bytesPerRow {
    ++g_base_calls;
}
@end

@interface PulpRecorderFakeTexture2CallsSuper : PulpRecorderFakeTexture2
@end
@implementation PulpRecorderFakeTexture2CallsSuper
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
            withBytes:(const void*)bytes
          bytesPerRow:(NSUInteger)bytesPerRow {
    enter_override();
    if (g_override_depth == 1)
        [super replaceRegion:region mipmapLevel:level withBytes:bytes bytesPerRow:bytesPerRow];
    --g_override_depth;
}
@end

using pulp::testing::TextureUploadRecord;
using pulp::testing::TextureUploadRecorder;

namespace {

// Independent of the recorder's own hashing.
std::string sha256_hex(const std::string& bytes) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
    std::string hex;
    char two[3];
    for (const auto b : digest) {
        std::snprintf(two, sizeof two, "%02x", b);
        hex += two;
    }
    return hex;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// `rows` rows of `row_bytes` pixel bytes, each followed by `padding` bytes of 0xEE.
struct Source {
    std::string padded;
    std::string packed;
};

Source make_source(std::size_t row_bytes, std::size_t rows, std::size_t padding, unsigned seed) {
    Source s;
    for (std::size_t y = 0; y < rows; ++y) {
        for (std::size_t x = 0; x < row_bytes; ++x) {
            const char c = static_cast<char>((seed * 31 + y * 17 + x * 7) & 0xFF);
            s.padded += c;
            s.packed += c;
        }
        s.padded.append(padding, static_cast<char>(0xEE));
    }
    return s;
}

struct Run {
    std::vector<TextureUploadRecord> records;
    std::filesystem::path dir;
    Source a, b, c;
    std::string a_back, b_back, c_back;
};

// Uploads the three textures with the recorder installed into `dir`.
// `poke` changes one pixel byte of texture B before upload.
Run upload_three(const std::filesystem::path& dir, bool poke) {
    Run run;
    run.dir = dir;
    run.a = make_source(4 * 4, 3, 0, 1);  // RGBA8 4x3, tight rows
    run.b = make_source(5 * 4, 2, 12, 2); // BGRA8 5x2, rows padded to 32 bytes
    run.c = make_source(3 * 4, 2, 4, 3);  // RGBA8 3x2 through a staging buffer, rows padded to 16
    if (poke) {
        run.b.padded[5] = static_cast<char>(run.b.padded[5] ^ 0x01);
        run.b.packed[5] = run.b.padded[5];
    }

    TextureUploadRecorder::Options options;
    options.output_dir = dir;
    std::string error;
    REQUIRE(TextureUploadRecorder::install(options, &error));
    REQUIRE(error.empty());

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        REQUIRE(device != nil);

        MTLTextureDescriptor* da =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:4
                                                              height:3
                                                           mipmapped:NO];
        da.storageMode = MTLStorageModeShared;
        id<MTLTexture> ta = [device newTextureWithDescriptor:da];
        [ta replaceRegion:MTLRegionMake2D(0, 0, 4, 3)
              mipmapLevel:0
                withBytes:run.a.padded.data()
              bytesPerRow:16];

        MTLTextureDescriptor* db =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:5
                                                              height:2
                                                           mipmapped:NO];
        db.storageMode = MTLStorageModeShared;
        id<MTLTexture> tb = [device newTextureWithDescriptor:db];
        [tb replaceRegion:MTLRegionMake2D(0, 0, 5, 2)
              mipmapLevel:0
                    slice:0
                withBytes:run.b.padded.data()
              bytesPerRow:32
            bytesPerImage:64];

        MTLTextureDescriptor* dc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:3
                                                              height:2
                                                           mipmapped:NO];
        dc.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tc = [device newTextureWithDescriptor:dc];
        id<MTLBuffer> staging = [device newBufferWithBytes:run.c.padded.data()
                                                    length:run.c.padded.size()
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> readback = [device newBufferWithLength:run.c.packed.size()
                                                     options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromBuffer:staging
                   sourceOffset:0
              sourceBytesPerRow:16
            sourceBytesPerImage:32
                     sourceSize:MTLSizeMake(3, 2, 1)
                      toTexture:tc
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        // Read C back through a blit the recorder does not wrap (texture to buffer).
        [blit copyFromTexture:tc
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(3, 2, 1)
                            toBuffer:readback
                   destinationOffset:0
              destinationBytesPerRow:12
            destinationBytesPerImage:24];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];

        TextureUploadRecorder::stop();

        // An upload after stop() is not recorded.
        [ta replaceRegion:MTLRegionMake2D(0, 0, 4, 3)
              mipmapLevel:0
                withBytes:run.a.padded.data()
              bytesPerRow:16];

        run.a_back.resize(run.a.packed.size());
        [ta getBytes:run.a_back.data()
            bytesPerRow:16
             fromRegion:MTLRegionMake2D(0, 0, 4, 3)
            mipmapLevel:0];
        run.b_back.resize(run.b.packed.size());
        [tb getBytes:run.b_back.data()
            bytesPerRow:20
             fromRegion:MTLRegionMake2D(0, 0, 5, 2)
            mipmapLevel:0];
        run.c_back.assign(static_cast<const char*>(readback.contents), run.c.packed.size());
    }
    run.records = TextureUploadRecorder::records();
    return run;
}

struct Scratch {
    std::filesystem::path root = pulp::test::make_unique_temp_dir("pulp_texture_upload_recorder");
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

} // namespace

TEST_CASE("TextureUploadRecorder records exactly the uploads, byte for byte",
          "[texture-upload-recorder]") {
    Scratch scratch;
    const auto run = upload_three(scratch.root / "run", false);

    // The textures hold what was uploaded: recording changed nothing.
    CHECK(run.a_back == run.a.packed);
    CHECK(run.b_back == run.b.packed);
    CHECK(run.c_back == run.c.packed);

    REQUIRE(run.records.size() == 3);
    const auto& a = run.records[0];
    const auto& b = run.records[1];
    const auto& c = run.records[2];

    CHECK(a.entry == "replaceRegion");
    CHECK(b.entry == "replaceRegion:slice");
    CHECK(c.entry == "copyFromBuffer");
    CHECK(a.pixel_format == "RGBA8Unorm");
    CHECK(b.pixel_format == "BGRA8Unorm");
    CHECK(c.pixel_format == "RGBA8Unorm");
    CHECK((a.width == 4 && a.height == 3));
    CHECK((b.width == 5 && b.height == 2));
    CHECK((c.width == 3 && c.height == 2));
    CHECK(b.source_bytes_per_row == 32);
    CHECK(c.source_bytes_per_row == 16);
    CHECK((a.texture_id != 0 && b.texture_id != 0 && c.texture_id != 0));
    CHECK((a.texture_id != b.texture_id && b.texture_id != c.texture_id));

    // Hashes and raw files cover the pixels with the row padding stripped.
    const Source* sources[] = {&run.a, &run.b, &run.c};
    for (int i = 0; i < 3; ++i) {
        const auto& r = run.records[static_cast<std::size_t>(i)];
        CAPTURE(i);
        CHECK(r.seq == static_cast<std::uint64_t>(i + 1));
        CHECK(r.api == "metal");
        CHECK(r.padding_stripped);
        CHECK(r.byte_length == sources[i]->packed.size());
        CHECK(r.sha256 == sha256_hex(sources[i]->packed));
        REQUIRE_FALSE(r.raw_file.empty());
        CHECK(slurp(run.dir / r.raw_file) == sources[i]->packed);
        CHECK_FALSE(r.caller_image.empty());
    }

    // The manifest has one line per upload, naming each hash.
    const auto manifest = slurp(run.dir / "manifest.jsonl");
    CHECK(std::count(manifest.begin(), manifest.end(), '\n') == 3);
    for (const auto& r : run.records)
        CHECK(manifest.find("\"sha256\":\"" + r.sha256 + "\"") != std::string::npos);
}

TEST_CASE("TextureUploadRecorder reports one changed byte as a different hash",
          "[texture-upload-recorder]") {
    Scratch scratch;
    const auto same = upload_three(scratch.root / "same", false);
    const auto poked = upload_three(scratch.root / "poked", true);
    REQUIRE(same.records.size() == 3);
    REQUIRE(poked.records.size() == 3);
    CHECK(poked.records[0].sha256 == same.records[0].sha256);
    CHECK(poked.records[1].sha256 != same.records[1].sha256);
    CHECK(poked.records[2].sha256 == same.records[2].sha256);
}

TEST_CASE("TextureUploadRecorder refuses an output directory inside a repository",
          "[texture-upload-recorder]") {
    // A scratch working tree: a directory holding a .git entry, as a checkout
    // does. Nothing here touches the real checkout.
    Scratch scratch;
    std::filesystem::create_directories(scratch.root / ".git");
    const auto dir = scratch.root / "recordings" / "run";
    struct StopRecording {
        ~StopRecording() {
            TextureUploadRecorder::stop();
        }
    } stop_recording;

    TextureUploadRecorder::Options options;
    options.output_dir = dir;
    std::string error;
    CHECK_FALSE(TextureUploadRecorder::install(options, &error));
    CHECK(error.find("git working tree") != std::string::npos);
    CHECK_FALSE(TextureUploadRecorder::recording());
    // A refusal creates nothing.
    CHECK_FALSE(std::filesystem::exists(scratch.root / "recordings"));

    // Control: the same directory outside a working tree is accepted.
    std::filesystem::remove_all(scratch.root / ".git");
    CHECK(TextureUploadRecorder::install(options, &error));
    CHECK(std::filesystem::exists(dir / "manifest.jsonl"));
}

TEST_CASE("TextureUploadRecorder hooks reach each original once through subclasses",
          "[texture-upload-recorder]") {
    using pulp::testing::detail::hook_texture_class_for_testing;
    TextureUploadRecorder::stop();
    const char bytes[16] = {};

    auto upload = [&](id texture) {
        g_base_calls = 0;
        g_override_calls = 0;
        g_override_depth = 0;
        g_override_reentered = false;
        [texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                   mipmapLevel:0
                     withBytes:bytes
                   bytesPerRow:4];
    };

    SECTION("base hooked before its subclasses") {
        hook_texture_class_for_testing([PulpRecorderFakeTexture class]);
        hook_texture_class_for_testing([PulpRecorderFakeTextureInherits class]);
        hook_texture_class_for_testing([PulpRecorderFakeTextureCallsSuper class]);

        upload([PulpRecorderFakeTexture new]);
        CHECK(g_base_calls == 1);

        // Inherits the wrapped method from an already hooked class.
        upload([PulpRecorderFakeTextureInherits new]);
        CHECK(g_base_calls == 1);

        // Its own original calls super, which is wrapped too.
        upload([PulpRecorderFakeTextureCallsSuper new]);
        CHECK_FALSE(g_override_reentered);
        CHECK(g_override_calls == 1);
        CHECK(g_base_calls == 1);
    }

    SECTION("subclass hooked before its base") {
        hook_texture_class_for_testing([PulpRecorderFakeTexture2CallsSuper class]);
        hook_texture_class_for_testing([PulpRecorderFakeTexture2 class]);

        upload([PulpRecorderFakeTexture2CallsSuper new]);
        CHECK_FALSE(g_override_reentered);
        CHECK(g_override_calls == 1);
        CHECK(g_base_calls == 1);

        upload([PulpRecorderFakeTexture2 new]);
        CHECK(g_override_calls == 0);
        CHECK(g_base_calls == 1);
    }
}

TEST_CASE("TextureUploadRecorder stops and removes the manifest when a write fails",
          "[texture-upload-recorder]") {
    Scratch scratch;
    const auto dir = scratch.root / "run";
    TextureUploadRecorder::Options options;
    options.output_dir = dir;
    options.write_raw = false;
    std::string error;
    REQUIRE(TextureUploadRecorder::install(options, &error));
    CHECK(TextureUploadRecorder::failure().empty());

    const auto source = make_source(4 * 4, 3, 0, 9);
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        REQUIRE(device != nil);
        MTLTextureDescriptor* d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:4
                                                              height:3
                                                           mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [device newTextureWithDescriptor:d];
        REQUIRE(texture != nil);
        auto upload = [&] {
            [texture replaceRegion:MTLRegionMake2D(0, 0, 4, 3)
                       mipmapLevel:0
                         withBytes:source.packed.data()
                       bytesPerRow:16];
        };

        // Control: the first upload is written whole.
        upload();
        REQUIRE(TextureUploadRecorder::records().size() == 1);
        const auto written = std::filesystem::file_size(dir / "manifest.jsonl");
        REQUIRE(written > 0);

        // Let the manifest grow by only a few bytes, so the second line is cut
        // short, as on a full disk. The limit makes the write fail with EFBIG
        // instead of raising SIGXFSZ.
        struct rlimit saved{};
        REQUIRE(getrlimit(RLIMIT_FSIZE, &saved) == 0);
        const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
        struct rlimit tight = saved;
        tight.rlim_cur = static_cast<rlim_t>(written + 8);
        REQUIRE(setrlimit(RLIMIT_FSIZE, &tight) == 0);
        upload();
        setrlimit(RLIMIT_FSIZE, &saved);
        std::signal(SIGXFSZ, previous_handler);

        // Later uploads are not recorded either.
        upload();
    }

    CHECK_FALSE(TextureUploadRecorder::recording());
    CHECK(TextureUploadRecorder::failure().find("cannot write") != std::string::npos);
    CHECK(TextureUploadRecorder::records().size() == 1);
    CHECK_FALSE(std::filesystem::exists(dir / "manifest.jsonl"));
    TextureUploadRecorder::stop();
}
