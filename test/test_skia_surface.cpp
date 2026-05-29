#include <catch2/catch_test_macros.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skp_capture.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkSerialProcs.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#endif

using namespace pulp::render;

TEST_CASE("SkiaSurface requires initialized GpuSurface", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    // Don't initialize GpuSurface — SkiaSurface should fail gracefully
    SkiaSurface::Config config{};
    config.width = 400;
    config.height = 300;

    auto skia = SkiaSurface::create(*gpu, config);
    // Should be null because GpuSurface has no Dawn handles
    REQUIRE(skia == nullptr);
#else
    REQUIRE(true);  // Skia not compiled in
#endif
}

TEST_CASE("SkiaSurface uses shared GpuSurface device", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    GpuSurface::Config gpu_config{};
    gpu_config.width = 400;
    gpu_config.height = 300;

    if (!gpu->initialize(gpu_config)) return;  // no GPU adapter

    SkiaSurface::Config config{};
    config.width = 400;
    config.height = 300;
    config.scale_factor = 1.0f;

    auto skia = SkiaSurface::create(*gpu, config);
    if (!skia) return;  // Graphite context creation failed

    REQUIRE(skia->is_available());
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface offscreen frame cycle", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    GpuSurface::Config gpu_config{};
    gpu_config.width = 200;
    gpu_config.height = 150;

    if (!gpu->initialize(gpu_config)) return;

    SkiaSurface::Config config{};
    config.width = 200;
    config.height = 150;

    auto skia = SkiaSurface::create(*gpu, config);
    if (!skia || !skia->is_available()) return;

    // Frame cycle: GpuSurface brackets the frame, SkiaSurface draws
    REQUIRE(gpu->begin_frame());

    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);

    // Draw something
    canvas->set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
    canvas->fill_rect(0, 0, 200, 150);

    skia->end_frame();  // submit Graphite recording
    gpu->end_frame();   // present (no-op in offscreen mode)
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface multiple frame cycles", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    GpuSurface::Config gpu_config{};
    gpu_config.width = 100;
    gpu_config.height = 100;

    if (!gpu->initialize(gpu_config)) return;

    auto skia = SkiaSurface::create(*gpu, {.width = 100, .height = 100});
    if (!skia || !skia->is_available()) return;

    // Multiple frames — verify no state leaks
    for (int i = 0; i < 5; ++i) {
        REQUIRE(gpu->begin_frame());
        auto* canvas = skia->begin_frame();
        REQUIRE(canvas != nullptr);
        canvas->fill_rect(0, 0, 100, 100);
        skia->end_frame();
        gpu->end_frame();
    }
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface resize", "[render][skia]") {
#ifdef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;

    GpuSurface::Config gpu_config{};
    gpu_config.width = 200;
    gpu_config.height = 200;

    if (!gpu->initialize(gpu_config)) return;

    auto skia = SkiaSurface::create(*gpu, {.width = 200, .height = 200});
    if (!skia || !skia->is_available()) return;

    // Resize both GpuSurface and SkiaSurface
    gpu->resize(400, 300);
    skia->resize(400, 300);

    // Should still work after resize
    REQUIRE(gpu->begin_frame());
    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    canvas->fill_rect(0, 0, 400, 300);
    skia->end_frame();
    gpu->end_frame();
#else
    REQUIRE(true);
#endif
}

TEST_CASE("SkiaSurface null without Skia", "[render][skia]") {
#ifndef PULP_HAS_SKIA
    auto gpu = GpuSurface::create_dawn();
    if (gpu) {
        auto skia = SkiaSurface::create(*gpu, {});
        REQUIRE(skia == nullptr);
    }
#else
    REQUIRE(true);  // Skia is available, tested elsewhere
#endif
}

// ---------------------------------------------------------------------------
// SkPicture (.skp) serialization on the Graphite backend
//
// Phase 6.4 spike (inspector roadmap): the planned "capture Skia frame"
// feature wants to write a `.skp` artifact that the Skia team's own
// `skiadebugger` can replay. Pulp renders through the Graphite backend
// (skgpu::graphite::Context) — `.skp` / SkPicture is the legacy
// SkPicture serialization format, so we must verify the two compose.
//
// Verdict from these tests: `SkPicture` is backend-independent. An
// `SkPictureRecorder` records into a backend-agnostic `SkRecord`; nothing
// in the Graphite headers references `SkPicture`. So `serialize()` /
// `MakeFromData()` round-trips cleanly regardless of which GPU backend the
// process is running. The single caveat is documented in SkPicture.h: the
// default serializer encodes embedded `SkImage`s as nullptr unless the
// caller supplies `SkSerialProcs::fImageProc`. That is format policy, not a
// Graphite limitation — Phase 6.4 must set fImageProc to capture frames
// that embed images. These tests pin both facts.
// ---------------------------------------------------------------------------
#ifdef PULP_HAS_SKIA

namespace {

// Record a small, deterministic vector scene (no embedded images) into an
// SkPicture. SkPictureRecorder is GPU-backend-agnostic by construction.
sk_sp<SkPicture> record_vector_scene() {
    SkPictureRecorder recorder;
    SkCanvas* rec = recorder.beginRecording(SkRect::MakeWH(64.0f, 48.0f));
    REQUIRE(rec != nullptr);

    SkPaint red;
    red.setColor(SK_ColorRED);
    red.setAntiAlias(true);
    rec->drawRect(SkRect::MakeXYWH(4.0f, 4.0f, 40.0f, 24.0f), red);

    SkPaint blue;
    blue.setColor(SK_ColorBLUE);
    rec->drawCircle(32.0f, 24.0f, 12.0f, blue);

    return recorder.finishRecordingAsPicture();
}

// Replay a picture into a fresh raster surface (pre-cleared to black) and
// read back the top-left pixel. cullRect equality only proves the picture
// *structure* survived serialization; replaying and sampling a pixel is
// what proves an embedded image's payload actually round-tripped.
SkColor replay_top_left(const sk_sp<SkPicture>& picture,
                        const SkImageInfo& info) {
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    surface->getCanvas()->clear(SK_ColorBLACK);
    surface->getCanvas()->drawPicture(picture.get());

    SkBitmap bm;
    REQUIRE(bm.tryAllocPixels(info));
    REQUIRE(surface->readPixels(bm, 0, 0));
    return bm.getColor(0, 0);
}

}  // namespace

TEST_CASE("SkPicture round-trips through serialize/MakeFromData", "[render][skia][skp]") {
    sk_sp<SkPicture> picture = record_vector_scene();
    REQUIRE(picture != nullptr);

    const SkRect bounds = picture->cullRect();
    REQUIRE(bounds.width() == 64.0f);
    REQUIRE(bounds.height() == 48.0f);

    // Serialize to an in-memory .skp blob.
    sk_sp<SkData> blob = picture->serialize();
    REQUIRE(blob != nullptr);
    REQUIRE(blob->size() > 0);

    // The blob carries the documented .skp file signature ("skiapict"),
    // which is what skiadebugger keys on to recognize the artifact.
    REQUIRE(blob->size() >= 8);
    REQUIRE(std::memcmp(blob->data(), "skiapict", 8) == 0);

    // Deserialize and assert the round-trip preserved the recording.
    sk_sp<SkPicture> restored = SkPicture::MakeFromData(blob.get());
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == bounds);
    REQUIRE(restored->approximateOpCount() == picture->approximateOpCount());
    REQUIRE(restored->approximateOpCount() > 0);
}

TEST_CASE("SkPicture round-trips through an SkStream", "[render][skia][skp]") {
    sk_sp<SkPicture> picture = record_vector_scene();
    REQUIRE(picture != nullptr);

    // serialize(SkWStream*) is the path Phase 6.4 will use to write a file.
    SkDynamicMemoryWStream out;
    picture->serialize(&out);
    REQUIRE(out.bytesWritten() > 0);

    sk_sp<SkData> blob = out.detachAsData();
    REQUIRE(blob != nullptr);

    SkMemoryStream in(blob);
    sk_sp<SkPicture> restored = SkPicture::MakeFromStream(&in);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == picture->cullRect());
    REQUIRE(restored->approximateOpCount() == picture->approximateOpCount());
}

TEST_CASE("SkPicture serialization is independent of any GPU context", "[render][skia][skp]") {
    // The whole point of the Phase 6.4 spike: prove SkPicture works without
    // — and regardless of — a live Graphite context. A picture recorded and
    // serialized with no GPU context whatsoever still round-trips, which is
    // exactly why it is safe on the Graphite backend (Graphite never touches
    // the SkPicture pipeline).
    sk_sp<SkPicture> picture = record_vector_scene();
    sk_sp<SkData> blob = picture->serialize();
    REQUIRE(blob != nullptr);

    sk_sp<SkPicture> restored = SkPicture::MakeFromData(blob->data(), blob->size());
    REQUIRE(restored != nullptr);
    REQUIRE(restored->uniqueID() != 0);
    REQUIRE(restored->approximateOpCount() == picture->approximateOpCount());
}

TEST_CASE("SkPicture with embedded image needs fImageProc to round-trip pixels",
          "[render][skia][skp]") {
    // SkPicture.h documents: "The default behavior for serializing SkImages
    // is to encode a nullptr." This is the one real caveat for Phase 6.4 —
    // a captured frame that embeds images must supply SkSerialProcs so the
    // image survives the round trip. This test pins that contract so the
    // Phase 6.4 implementation does not silently ship null-image captures.

    // A tiny raster image (the kind a real frame would embed; Graphite frames
    // upload raster sources, so this mirrors the captured-frame case).
    SkImageInfo info = SkImageInfo::MakeN32Premul(8, 8);
    sk_sp<SkSurface> raster = SkSurfaces::Raster(info);
    REQUIRE(raster != nullptr);
    raster->getCanvas()->clear(SK_ColorGREEN);
    sk_sp<SkImage> image = raster->makeImageSnapshot();
    REQUIRE(image != nullptr);

    SkPictureRecorder recorder;
    SkCanvas* rec = recorder.beginRecording(SkRect::MakeWH(8.0f, 8.0f));
    rec->drawImage(image, 0.0f, 0.0f);
    sk_sp<SkPicture> picture = recorder.finishRecordingAsPicture();
    REQUIRE(picture != nullptr);

    // (a) Default serialization: the picture structure survives, the
    //     embedded image is dropped to null. Still a valid .skp.
    {
        sk_sp<SkData> blob = picture->serialize();
        REQUIRE(blob != nullptr);
        sk_sp<SkPicture> restored = SkPicture::MakeFromData(blob.get());
        REQUIRE(restored != nullptr);
        REQUIRE(restored->cullRect() == picture->cullRect());

        // The image payload did NOT survive: replaying the restored
        // picture draws nothing where the image was, so the surface
        // keeps its black clear color. This is the contrast that makes
        // the (b) pixel assertion below meaningful.
        REQUIRE(replay_top_left(restored, info) == SK_ColorBLACK);
    }

    // (b) With matching SkSerialProcs/SkDeserialProcs fImageProc set, the
    //     embedded image round-trips through serialize + MakeFromData.
    {
        SkSerialProcs sprocs;
        // Match the SkSerialImageProc return type selected by the packaged
        // Skia headers.
        sprocs.fImageProc = [](SkImage* img, void*) -> SkSerialReturnType {
            return SkPngEncoder::Encode(nullptr, img, SkPngEncoder::Options{});
        };
        sk_sp<SkData> blob = picture->serialize(&sprocs);
        REQUIRE(blob != nullptr);

        SkDeserialProcs dprocs;
        dprocs.fImageProc = [](const void* data, size_t length,
                               void*) -> sk_sp<SkImage> {
            return SkImages::DeferredFromEncodedData(
                SkData::MakeWithCopy(data, length));
        };
        sk_sp<SkPicture> restored = SkPicture::MakeFromData(blob.get(), &dprocs);
        REQUIRE(restored != nullptr);
        REQUIRE(restored->cullRect() == picture->cullRect());

        // The image payload survived: replaying the restored picture
        // reproduces the original green image. cullRect equality alone
        // would still pass if SkPngEncoder::Encode had returned null and
        // the image deserialized to nothing — this pixel check is what
        // actually proves fImageProc preserved the pixels.
        REQUIRE(replay_top_left(restored, info) == SK_ColorGREEN);
    }
}

#endif  // PULP_HAS_SKIA

// ---------------------------------------------------------------------------
// SkpFrameCapture — Phase 6.4 `.skp` frame-capture API (core/render)
//
// These exercise the public capture surface in
// core/render/include/pulp/render/skp_capture.hpp: record a frame's
// draw ops through the capture's pulp::canvas::Canvas, serialize a
// `.skp` artifact, and prove the round trip — including the load-bearing
// embedded-image-pixel assertion the spike pinned (cullRect equality
// alone is insufficient). The capture path is GPU-backend-agnostic and
// needs no live GPU context, so these run on the CI matrix even without
// a GPU adapter.
// ---------------------------------------------------------------------------

#ifdef PULP_HAS_SKIA

namespace {

// Deserialize-side image proc — the replay-side half of the contract
// SkpFrameCapture's fImageProc establishes. Decodes the PNG payload
// SkpFrameCapture wrote back into an SkImage.
SkDeserialProcs skp_deserial_procs() {
    SkDeserialProcs procs;
    procs.fImageProc = [](const void* data, size_t length,
                          void*) -> sk_sp<SkImage> {
        return SkImages::DeferredFromEncodedData(
            SkData::MakeWithCopy(data, length));
    };
    return procs;
}

// A tiny solid-color PNG, the kind a real frame embeds via the image
// atlas. Encoded here so draw_image_from_data() has a decodable source.
sk_sp<SkData> solid_png(int size, SkColor color) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(size, size);
    sk_sp<SkSurface> raster = SkSurfaces::Raster(info);
    REQUIRE(raster != nullptr);
    raster->getCanvas()->clear(color);
    sk_sp<SkImage> image = raster->makeImageSnapshot();
    REQUIRE(image != nullptr);
    return SkPngEncoder::Encode(nullptr, image.get(), SkPngEncoder::Options{});
}

}  // namespace

TEST_CASE("SkpFrameCapture records vector ops into a round-tripping .skp",
          "[render][skia][skp]") {
    pulp::render::SkpFrameCapture capture(64, 48);
    REQUIRE(capture.available());
    REQUIRE(pulp::render::skp_capture_supported());

    pulp::canvas::Canvas* c = capture.canvas();
    REQUIRE(c != nullptr);
    c->set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
    c->fill_rect(4.0f, 4.0f, 40.0f, 24.0f);
    c->set_fill_color(pulp::canvas::Color::rgba8(0, 0, 255));
    c->fill_rect(20.0f, 20.0f, 24.0f, 16.0f);

    std::string blob;
    auto result = capture.finish_to_memory(blob);
    REQUIRE(result.ok);
    REQUIRE(result.bytes_written == blob.size());
    REQUIRE(result.op_count > 0);

    // The blob carries the documented .skp "skiapict" signature.
    REQUIRE(blob.size() >= 8);
    REQUIRE(std::memcmp(blob.data(), "skiapict", 8) == 0);

    // Deserialize and assert the recording survived.
    sk_sp<SkPicture> restored =
        SkPicture::MakeFromData(blob.data(), blob.size());
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == SkRect::MakeWH(64.0f, 48.0f));
    REQUIRE(restored->approximateOpCount() > 0);

    // The capture is consumed: canvas() is null, a second finish fails.
    REQUIRE(capture.canvas() == nullptr);
    REQUIRE_FALSE(capture.available());
    std::string second;
    REQUIRE_FALSE(capture.finish_to_memory(second).ok);
}

TEST_CASE("SkpFrameCapture writes a loadable .skp file", "[render][skia][skp]") {
    const std::string path =
        (std::filesystem::temp_directory_path() /
         "pulp-skp-capture-test.skp").string();
    std::filesystem::remove(path);

    auto result = pulp::render::capture_skp_to_file(
        32, 32, path, [](pulp::canvas::Canvas& c) {
            c.set_fill_color(pulp::canvas::Color::rgba8(0, 255, 0));
            c.fill_rect(0.0f, 0.0f, 32.0f, 32.0f);
        });
    REQUIRE(result.ok);
    REQUIRE(result.path == path);
    REQUIRE(result.bytes_written > 0);
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::file_size(path) == result.bytes_written);

    // Re-read the file off disk and confirm skiadebugger could load it.
    SkFILEStream in(path.c_str());
    REQUIRE(in.isValid());
    sk_sp<SkPicture> restored = SkPicture::MakeFromStream(&in);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == SkRect::MakeWH(32.0f, 32.0f));

    std::filesystem::remove(path);
}

TEST_CASE("SkpFrameCapture preserves embedded-image pixels via fImageProc",
          "[render][skia][skp]") {
    // The load-bearing Phase 6.4 assertion: a frame that embeds an image
    // must survive the .skp round trip with its pixels intact.
    // SkpFrameCapture sets SkSerialProcs::fImageProc (PNG encode); if it
    // did not, the embedded image would deserialize to null and the
    // replay would be blank. Replay + pixel-sample is what proves the
    // payload survived — cullRect equality alone would still pass.
    sk_sp<SkData> png = solid_png(8, SK_ColorGREEN);
    REQUIRE(png != nullptr);

    pulp::render::SkpFrameCapture capture(8, 8);
    REQUIRE(capture.available());
    REQUIRE(capture.canvas()->draw_image_from_data(
        static_cast<const uint8_t*>(png->data()), png->size(),
        0.0f, 0.0f, 8.0f, 8.0f));

    std::string blob;
    auto result = capture.finish_to_memory(blob);
    REQUIRE(result.ok);
    REQUIRE(result.op_count > 0);

    // Deserialize WITH the matching fImageProc and replay into a fresh
    // raster surface pre-cleared to black; sample the top-left pixel.
    SkDeserialProcs dprocs = skp_deserial_procs();
    sk_sp<SkPicture> restored =
        SkPicture::MakeFromData(blob.data(), blob.size(), &dprocs);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->cullRect() == SkRect::MakeWH(8.0f, 8.0f));

    SkImageInfo info = SkImageInfo::MakeN32Premul(8, 8);
    REQUIRE(replay_top_left(restored, info) == SK_ColorGREEN);
}

TEST_CASE("SkpFrameCapture round-trips a GPU-texture-backed embedded image",
          "[render][skia][skp]") {
    // Codex P1: SkPicture::serialize()'s image proc must not silently drop
    // GPU-texture-backed embedded images. SkpFrameCapture rasterizes them
    // via the Graphite Context threaded through the capture, so a frame
    // that embeds a GPU image survives the .skp round trip with pixels
    // intact. Requires a live GPU adapter — skips cleanly without one.
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) return;
    GpuSurface::Config gpu_config{};
    gpu_config.width = 16;
    gpu_config.height = 16;
    if (!gpu->initialize(gpu_config)) return;  // no GPU adapter

    auto skia = SkiaSurface::create(*gpu, {.width = 16, .height = 16});
    if (!skia || !skia->is_available()) return;

    skgpu::graphite::Context* ctx = skia->graphite_context();
    REQUIRE(ctx != nullptr);

    // Build a GPU-texture-backed image: render solid green into an
    // offscreen Graphite render target and snapshot it. A Graphite
    // surface snapshot is a texture-backed SkImage — exactly the
    // atlas/snapshot case the fix targets. The recording must be
    // inserted + submitted so the texture actually holds the pixels.
    std::unique_ptr<skgpu::graphite::Recorder> recorder = ctx->makeRecorder();
    REQUIRE(recorder != nullptr);
    SkImageInfo info = SkImageInfo::MakeN32Premul(8, 8);
    sk_sp<SkSurface> gpu_surface =
        SkSurfaces::RenderTarget(recorder.get(), info);
    if (!gpu_surface) return;  // offscreen GPU target unavailable
    gpu_surface->getCanvas()->clear(SK_ColorGREEN);
    sk_sp<SkImage> gpu_image = gpu_surface->makeImageSnapshot();
    REQUIRE(gpu_image != nullptr);
    REQUIRE(gpu_image->isTextureBacked());
    {
        std::unique_ptr<skgpu::graphite::Recording> rec = recorder->snap();
        skgpu::graphite::InsertRecordingInfo rec_info{};
        rec_info.fRecording = rec.get();
        REQUIRE(ctx->insertRecording(rec_info) ==
                skgpu::graphite::InsertStatus::kSuccess);
        ctx->submit(skgpu::graphite::SyncToCpu::kYes);
    }

    pulp::render::SkpFrameCapture capture(8, 8, ctx);
    REQUIRE(capture.available());
    // Draw the texture-backed image into the capture canvas.
    auto* skia_canvas =
        static_cast<pulp::canvas::SkiaCanvas*>(capture.canvas());
    REQUIRE(skia_canvas != nullptr);
    REQUIRE(skia_canvas->draw_skia_image(gpu_image, 0.0f, 0.0f, 8.0f, 8.0f));

    std::string blob;
    auto result = capture.finish_to_memory(blob);
    REQUIRE(result.ok);
    REQUIRE(result.op_count > 0);

    // Deserialize with the matching image proc and replay. If the texture
    // image had been dropped, the replay would be black, not green.
    SkDeserialProcs dprocs = skp_deserial_procs();
    sk_sp<SkPicture> restored =
        SkPicture::MakeFromData(blob.data(), blob.size(), &dprocs);
    REQUIRE(restored != nullptr);
    REQUIRE(replay_top_left(restored, info) == SK_ColorGREEN);
}

TEST_CASE("SkpFrameCapture writes atomically — a failed write leaves no file",
          "[render][skia][skp]") {
    // Codex P2: the .skp must never land half-written and must never
    // clobber a previously-valid capture. The write goes to a sibling
    // <dest>.tmp and is renamed onto the destination only on full
    // success.

    SECTION("failed write does not create the destination file") {
        // A path inside a directory that does not exist: opening the
        // sibling temp file fails, so nothing is written.
        const std::string bad_dir =
            (std::filesystem::temp_directory_path() /
             "pulp-skp-no-such-dir-xyz").string();
        std::filesystem::remove_all(bad_dir);
        const std::string dest = bad_dir + "/frame.skp";

        auto result = pulp::render::capture_skp_to_file(
            16, 16, dest, [](pulp::canvas::Canvas& c) {
                c.set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0));
                c.fill_rect(0.0f, 0.0f, 16.0f, 16.0f);
            });
        REQUIRE_FALSE(result.ok);
        REQUIRE_FALSE(result.reason.empty());
        REQUIRE_FALSE(std::filesystem::exists(dest));
        REQUIRE_FALSE(std::filesystem::exists(dest + ".tmp"));
    }

    SECTION("a failed write leaves a prior valid capture intact") {
        const std::string path =
            (std::filesystem::temp_directory_path() /
             "pulp-skp-atomic-prior.skp").string();
        std::filesystem::remove(path);

        // First capture succeeds and writes a valid .skp.
        auto first = pulp::render::capture_skp_to_file(
            16, 16, path, [](pulp::canvas::Canvas& c) {
                c.set_fill_color(pulp::canvas::Color::rgba8(0, 255, 0));
                c.fill_rect(0.0f, 0.0f, 16.0f, 16.0f);
            });
        REQUIRE(first.ok);
        REQUIRE(std::filesystem::exists(path));
        const auto original_size = std::filesystem::file_size(path);
        REQUIRE(original_size > 0);

        // A second capture aimed at the same path but with a temp file
        // that cannot be created: pre-create the <dest>.tmp path as a
        // NON-EMPTY directory. SkFILEWStream cannot open a directory as a
        // file, and the non-empty directory also survives the unlink
        // attempt — so the write fails and the destination is untouched.
        const std::string tmp_as_dir = path + ".tmp";
        std::filesystem::remove_all(tmp_as_dir);
        std::filesystem::create_directory(tmp_as_dir);
        { std::ofstream guard(tmp_as_dir + "/keep"); guard << "x"; }

        auto second = pulp::render::capture_skp_to_file(
            16, 16, path, [](pulp::canvas::Canvas& c) {
                c.set_fill_color(pulp::canvas::Color::rgba8(0, 0, 255));
                c.fill_rect(0.0f, 0.0f, 16.0f, 16.0f);
            });
        REQUIRE_FALSE(second.ok);
        REQUIRE_FALSE(second.reason.empty());

        // The prior capture is untouched — same file, same bytes.
        REQUIRE(std::filesystem::exists(path));
        REQUIRE(std::filesystem::file_size(path) == original_size);
        SkFILEStream in(path.c_str());
        REQUIRE(in.isValid());
        sk_sp<SkPicture> restored = SkPicture::MakeFromStream(&in);
        REQUIRE(restored != nullptr);

        std::filesystem::remove_all(tmp_as_dir);
        std::filesystem::remove(path);
    }
}

TEST_CASE("SkpFrameCapture degrades gracefully on invalid input",
          "[render][skia][skp]") {
    // Non-positive dimensions yield an unavailable capture — no canvas,
    // no file, a clear reason. Never a crash or a partial .skp.
    pulp::render::SkpFrameCapture bad(0, 32);
    REQUIRE_FALSE(bad.available());
    REQUIRE(bad.canvas() == nullptr);

    std::string blob;
    auto mem = bad.finish_to_memory(blob);
    REQUIRE_FALSE(mem.ok);
    REQUIRE_FALSE(mem.reason.empty());
    REQUIRE(blob.empty());

    auto file = pulp::render::capture_skp_to_file(
        -1, -1, "/tmp/should-not-be-written.skp",
        [](pulp::canvas::Canvas&) {});
    REQUIRE_FALSE(file.ok);
    REQUIRE_FALSE(file.reason.empty());

    // Empty output path is rejected without writing.
    pulp::render::SkpFrameCapture ok(16, 16);
    REQUIRE(ok.available());
    auto empty_path = ok.finish_to_file("");
    REQUIRE_FALSE(empty_path.ok);
    REQUIRE_FALSE(empty_path.reason.empty());
}

#else  // !PULP_HAS_SKIA

TEST_CASE("SkpFrameCapture is unavailable without Skia", "[render][skia][skp]") {
    // The Skia-absent build must still link and degrade gracefully:
    // unavailable capture, null canvas, failed result with a reason.
    REQUIRE_FALSE(pulp::render::skp_capture_supported());

    pulp::render::SkpFrameCapture capture(64, 48);
    REQUIRE_FALSE(capture.available());
    REQUIRE(capture.canvas() == nullptr);

    std::string blob;
    auto mem = capture.finish_to_memory(blob);
    REQUIRE_FALSE(mem.ok);
    REQUIRE_FALSE(mem.reason.empty());

    auto file = pulp::render::capture_skp_to_file(
        64, 48, "/tmp/unused.skp", [](pulp::canvas::Canvas&) {});
    REQUIRE_FALSE(file.ok);
    REQUIRE_FALSE(file.reason.empty());
}

#endif  // PULP_HAS_SKIA
