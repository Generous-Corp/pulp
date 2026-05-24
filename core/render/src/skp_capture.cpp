#include <pulp/render/skp_capture.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/runtime/log.hpp>

#include <cstdio>
#include <memory>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSerialProcs.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/GraphiteTypes.h"

namespace pulp::render {

namespace {

// State carried through SkSerialProcs::fImageCtx — the optional Graphite
// Context the capture was constructed with. nullptr when the caller
// promised the frame is raster/vector-only.
struct SerialImageContext {
    skgpu::graphite::Context* graphite_context = nullptr;
};

// Receives the result of Context::asyncRescaleAndReadPixels.
struct ReadbackState {
    bool done = false;
    std::unique_ptr<const SkImage::AsyncReadResult> result;
};

// Rasterize a GPU-texture-backed image into CPU pixels via the Graphite
// Context's async readback, driven synchronously. Returns a raster
// SkImage on success, nullptr on failure. `.skp` capture is a
// user-triggered, off-the-audio-thread action, so the synchronous GPU
// flush here is acceptable (never call this from a realtime path).
sk_sp<SkImage> rasterize_texture_image(SkImage* image,
                                       skgpu::graphite::Context* ctx) {
    if (!image || !ctx) return nullptr;

    const SkImageInfo info = SkImageInfo::Make(
        image->width(), image->height(), kRGBA_8888_SkColorType,
        kPremul_SkAlphaType,
        image->refColorSpace() ? image->refColorSpace()
                               : SkColorSpace::MakeSRGB());

    ReadbackState state;
    ctx->asyncRescaleAndReadPixels(
        image, info, SkIRect::MakeWH(image->width(), image->height()),
        SkImage::RescaleGamma::kSrc, SkImage::RescaleMode::kNearest,
        [](SkImage::ReadPixelsContext c,
           std::unique_ptr<const SkImage::AsyncReadResult> r) {
            auto* s = static_cast<ReadbackState*>(c);
            s->result = std::move(r);
            s->done = true;
        },
        &state);

    // Drive the readback to completion on the CPU.
    ctx->submit(skgpu::graphite::SyncToCpu::kYes);
    while (!state.done) {
        ctx->checkAsyncWorkCompletion();
    }

    if (!state.result || state.result->count() < 1 ||
        !state.result->data(0)) {
        runtime::log_error(
            "SkpFrameCapture: GPU image readback failed — embedded image "
            "dropped from .skp");
        return nullptr;
    }

    // The AsyncReadResult owns the pixels only for its own lifetime, so
    // copy them into an SkData-backed raster image before it is freed.
    const SkPixmap pixmap(info, state.result->data(0),
                          state.result->rowBytes(0));
    return SkImages::RasterFromPixmapCopy(pixmap);
}

// Serialize-side image proc. SkPicture::serialize() drops embedded
// SkImages to nullptr by default (documented in SkPicture.h); this
// PNG-encodes them so atlas/image draws survive into the .skp. This is
// the contract the Phase 6.4 spike pinned in test_skia_surface.cpp.
//
// Raster images PNG-encode directly. A GPU-texture-backed image cannot:
// SkPngEncoder must read its pixels off the GPU first. We rasterize it
// via the Graphite Context threaded through fImageCtx. If a texture
// image arrives with no Context, the proc fails loudly (logged) instead
// of silently dropping it from the artifact.
// Skia m149: `SkSerialImageProc` return type tightened from
// `sk_sp<SkData>` to `sk_sp<const SkData>` (PNG/JPEG output buffers
// are immutable once encoded). Match the new signature exactly so
// the function-pointer assignment in `pulp_serial_procs` typechecks.
sk_sp<const SkData> encode_embedded_image(SkImage* image, void* ctx) {
    if (!image) return nullptr;

    if (image->isTextureBacked()) {
        auto* sctx = static_cast<SerialImageContext*>(ctx);
        skgpu::graphite::Context* gctx =
            sctx ? sctx->graphite_context : nullptr;
        if (!gctx) {
            runtime::log_error(
                "SkpFrameCapture: embedded image is GPU-texture-backed but "
                "no Graphite Context was supplied — pass the Context to "
                "SkpFrameCapture/capture_skp_to_file so it survives the .skp");
            return nullptr;
        }
        sk_sp<SkImage> raster = rasterize_texture_image(image, gctx);
        if (!raster) return nullptr;
        return SkPngEncoder::Encode(nullptr, raster.get(),
                                    SkPngEncoder::Options{});
    }

    return SkPngEncoder::Encode(nullptr, image, SkPngEncoder::Options{});
}

// Build the SkSerialProcs every Pulp .skp capture uses. `image_ctx`
// outlives the serialize() call it is passed to.
SkSerialProcs pulp_serial_procs(SerialImageContext* image_ctx) {
    SkSerialProcs procs;
    procs.fImageProc = &encode_embedded_image;
    procs.fImageCtx = image_ctx;
    return procs;
}

} // namespace

struct SkpFrameCapture::Impl {
    SkPictureRecorder recorder;
    std::unique_ptr<canvas::SkiaCanvas> canvas;
    bool consumed = false;
    int width = 0;
    int height = 0;
    skgpu::graphite::Context* graphite_context = nullptr;
};

SkpFrameCapture::SkpFrameCapture(int width, int height,
                                 skgpu::graphite::Context* graphite_context)
    : impl_(std::make_unique<Impl>()) {
    impl_->width = width;
    impl_->height = height;
    impl_->graphite_context = graphite_context;

    if (width <= 0 || height <= 0) {
        runtime::log_warn(
            "SkpFrameCapture: non-positive dimensions ({}x{}) — capture unavailable",
            width, height);
        return;
    }

    SkCanvas* rec = impl_->recorder.beginRecording(
        SkRect::MakeWH(static_cast<float>(width), static_cast<float>(height)));
    if (!rec) {
        runtime::log_error("SkpFrameCapture: beginRecording returned null");
        return;
    }

    // Wrap the recording canvas as a pulp::canvas::Canvas. No Graphite
    // recorder is passed: picture recording is GPU-backend-agnostic and
    // never touches the live GPU device.
    impl_->canvas = std::make_unique<canvas::SkiaCanvas>(rec);
}

SkpFrameCapture::~SkpFrameCapture() = default;

bool SkpFrameCapture::available() const {
    return impl_ && impl_->canvas != nullptr && !impl_->consumed;
}

canvas::Canvas* SkpFrameCapture::canvas() {
    if (!available()) return nullptr;
    return impl_->canvas.get();
}

SkpCaptureResult SkpFrameCapture::finish_to_memory(std::string& out_blob) {
    SkpCaptureResult result;
    out_blob.clear();

    if (!impl_ || !impl_->canvas) {
        result.reason = "skp capture unavailable (Skia missing or invalid size)";
        return result;
    }
    if (impl_->consumed) {
        result.reason = "skp capture already finished";
        return result;
    }
    impl_->consumed = true;

    // Drop the Canvas wrapper before finishing — finishRecordingAsPicture
    // invalidates the recording SkCanvas the wrapper points at.
    impl_->canvas.reset();

    sk_sp<SkPicture> picture = impl_->recorder.finishRecordingAsPicture();
    if (!picture) {
        result.reason = "finishRecordingAsPicture returned null";
        return result;
    }

    SerialImageContext image_ctx;
    image_ctx.graphite_context = impl_->graphite_context;
    SkSerialProcs procs = pulp_serial_procs(&image_ctx);
    sk_sp<SkData> blob = picture->serialize(&procs);
    if (!blob || blob->size() == 0) {
        result.reason = "SkPicture::serialize produced no data";
        return result;
    }

    out_blob.assign(static_cast<const char*>(blob->data()), blob->size());
    result.ok = true;
    result.bytes_written = blob->size();
    result.op_count = static_cast<std::size_t>(picture->approximateOpCount());
    return result;
}

SkpCaptureResult SkpFrameCapture::finish_to_file(const std::string& path) {
    SkpCaptureResult result;
    result.path = path;

    if (path.empty()) {
        result.reason = "skp capture: empty output path";
        // Still mark the capture consumed so a retry is honest.
        if (impl_) impl_->consumed = true;
        return result;
    }

    std::string blob;
    SkpCaptureResult mem = finish_to_memory(blob);
    if (!mem.ok) {
        result.reason = mem.reason;
        return result;
    }

    // Atomic write: serialize to a sibling temp file, then rename it onto
    // the destination only after the bytes are fully on disk. A failed
    // write therefore never leaves a truncated `.skp` and never clobbers
    // a previously-valid capture — the header's "never a half-written
    // file" guarantee.
    const std::string tmp_path = path + ".tmp";
    std::remove(tmp_path.c_str());

    {
        SkFILEWStream out(tmp_path.c_str());
        if (!out.isValid()) {
            result.reason = "could not open .skp temp file: " + tmp_path;
            return result;
        }
        if (!out.write(blob.data(), blob.size())) {
            result.reason = "failed writing .skp bytes to: " + tmp_path;
            // SkFILEWStream closes on scope exit; drop the partial temp
            // so the destination path is never touched.
            std::remove(tmp_path.c_str());
            return result;
        }
        out.fsync();
    } // SkFILEWStream destructor closes the file before the rename.

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        result.reason = "failed renaming .skp temp file onto: " + path;
        std::remove(tmp_path.c_str());
        return result;
    }

    result.ok = true;
    result.bytes_written = mem.bytes_written;
    result.op_count = mem.op_count;
    runtime::log_info("SkpFrameCapture: wrote {} byte .skp ({} ops) to {}",
                      result.bytes_written, result.op_count, path);
    return result;
}

SkpCaptureResult capture_skp_to_file(
    int width, int height, const std::string& path,
    const std::function<void(canvas::Canvas&)>& paint,
    skgpu::graphite::Context* graphite_context) {
    SkpFrameCapture capture(width, height, graphite_context);
    if (!capture.available()) {
        SkpCaptureResult result;
        result.path = path;
        result.reason = "skp capture unavailable (Skia missing or invalid size)";
        return result;
    }
    if (paint) {
        paint(*capture.canvas());
    }
    return capture.finish_to_file(path);
}

bool skp_capture_supported() { return true; }

} // namespace pulp::render

#else // !PULP_HAS_SKIA

#include <pulp/runtime/log.hpp>

namespace pulp::render {

// Skia-absent fallbacks. Every entry point degrades gracefully: no
// canvas, no file, a clear reason string — never a crash or a partial
// .skp artifact.

struct SkpFrameCapture::Impl {};

SkpFrameCapture::SkpFrameCapture(int /*width*/, int /*height*/,
                                 skgpu::graphite::Context* /*graphite_context*/)
    : impl_(nullptr) {}
SkpFrameCapture::~SkpFrameCapture() = default;

bool SkpFrameCapture::available() const { return false; }

canvas::Canvas* SkpFrameCapture::canvas() { return nullptr; }

SkpCaptureResult SkpFrameCapture::finish_to_file(const std::string& path) {
    SkpCaptureResult result;
    result.path = path;
    result.reason = "skp capture unavailable: this build has no Skia support";
    return result;
}

SkpCaptureResult SkpFrameCapture::finish_to_memory(std::string& out_blob) {
    out_blob.clear();
    SkpCaptureResult result;
    result.reason = "skp capture unavailable: this build has no Skia support";
    return result;
}

SkpCaptureResult capture_skp_to_file(
    int /*width*/, int /*height*/, const std::string& path,
    const std::function<void(canvas::Canvas&)>& /*paint*/,
    skgpu::graphite::Context* /*graphite_context*/) {
    SkpCaptureResult result;
    result.path = path;
    result.reason = "skp capture unavailable: this build has no Skia support";
    return result;
}

bool skp_capture_supported() { return false; }

} // namespace pulp::render

#endif // PULP_HAS_SKIA
