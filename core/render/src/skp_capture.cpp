#include <pulp/render/skp_capture.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/runtime/log.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSerialProcs.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"

namespace pulp::render {

namespace {

// Serialize-side image proc. SkPicture::serialize() drops embedded
// SkImages to nullptr by default (documented in SkPicture.h); this
// PNG-encodes them so atlas/image draws survive into the .skp. This is
// the contract the Phase 6.4 spike pinned in test_skia_surface.cpp.
sk_sp<SkData> encode_embedded_image(SkImage* image, void* /*ctx*/) {
    if (!image) return nullptr;
    return SkPngEncoder::Encode(nullptr, image, SkPngEncoder::Options{});
}

// Build the SkSerialProcs every Pulp .skp capture uses.
SkSerialProcs pulp_serial_procs() {
    SkSerialProcs procs;
    procs.fImageProc = &encode_embedded_image;
    return procs;
}

} // namespace

struct SkpFrameCapture::Impl {
    SkPictureRecorder recorder;
    std::unique_ptr<canvas::SkiaCanvas> canvas;
    bool consumed = false;
    int width = 0;
    int height = 0;
};

SkpFrameCapture::SkpFrameCapture(int width, int height)
    : impl_(std::make_unique<Impl>()) {
    impl_->width = width;
    impl_->height = height;

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

    SkSerialProcs procs = pulp_serial_procs();
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

    SkFILEWStream out(path.c_str());
    if (!out.isValid()) {
        result.reason = "could not open .skp output file: " + path;
        return result;
    }
    if (!out.write(blob.data(), blob.size())) {
        result.reason = "failed writing .skp bytes to: " + path;
        return result;
    }
    out.flush();

    result.ok = true;
    result.bytes_written = mem.bytes_written;
    result.op_count = mem.op_count;
    runtime::log_info("SkpFrameCapture: wrote {} byte .skp ({} ops) to {}",
                      result.bytes_written, result.op_count, path);
    return result;
}

SkpCaptureResult capture_skp_to_file(
    int width, int height, const std::string& path,
    const std::function<void(canvas::Canvas&)>& paint) {
    SkpFrameCapture capture(width, height);
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

SkpFrameCapture::SkpFrameCapture(int /*width*/, int /*height*/) : impl_(nullptr) {}
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
    const std::function<void(canvas::Canvas&)>& /*paint*/) {
    SkpCaptureResult result;
    result.path = path;
    result.reason = "skp capture unavailable: this build has no Skia support";
    return result;
}

bool skp_capture_supported() { return false; }

} // namespace pulp::render

#endif // PULP_HAS_SKIA
