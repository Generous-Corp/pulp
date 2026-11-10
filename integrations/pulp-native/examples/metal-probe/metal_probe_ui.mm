// The metal-probe editor: a bring-your-own-renderer editor on pulp-native.
// It owns a Metal device through MetalSurface, draws on a FramePump render
// thread, and presents its own drawables. Dragging vertically edits the gain
// as one gesture; the clear colour follows the gain and a triangle follows the
// pointer. Compiled with ARC.

#include "metal_probe_ui.hpp"

#include "metal_probe_processor.hpp"

#include <pulp/format/editor_host.hpp>
#include <pulp/native/frame_pump.hpp>
#include <pulp/native/metal_surface.hpp>

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#if !__has_feature(objc_arc)
#error "metal_probe_ui.mm must be compiled with -fobjc-arc"
#endif

namespace pulp_native_metal_probe {

namespace {

using namespace ::pulp::format;
using namespace ::pulp::native;

constexpr const char* kShaderSource = R"(
#include <metal_stdlib>
using namespace metal;
struct Probe { float2 centre; float2 half_size; };
vertex float4 probe_vertex(uint vid [[vertex_id]], constant Probe& p [[buffer(0)]]) {
    const float2 corners[3] = { float2(0.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0) };
    return float4(p.centre + corners[vid] * p.half_size, 0.0, 1.0);
}
fragment float4 probe_fragment() { return float4(1.0, 1.0, 1.0, 1.0); }
)";

struct ProbeUniforms {
    float centre[2];
    float half_size[2];
};

/// What one frame shows: the gain and the pointer, in surface points.
struct FrameInputs {
    float gain = 0.5f;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
};

/// Clear colour for a gain in [0, 1]: blue at 0, red at 1.
MTLClearColor clear_for(float gain) {
    return MTLClearColorMake(gain, 0.2, 1.0 - gain, 1.0);
}

class ProbeRenderer {
  public:
    bool init(id<MTLDevice> device, MTLPixelFormat format) {
        device_ = device;
        queue_ = [device newCommandQueue];
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(kShaderSource)
                                                      options:nil
                                                        error:&error];
        if (!library)
            return false;
        MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = [library newFunctionWithName:@"probe_vertex"];
        desc.fragmentFunction = [library newFunctionWithName:@"probe_fragment"];
        desc.colorAttachments[0].pixelFormat = format;
        pipeline_ = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        return pipeline_ != nil && queue_ != nil;
    }

    id<MTLCommandQueue> queue() const {
        return queue_;
    }

    void encode(id<MTLCommandBuffer> cb, id<MTLTexture> target, const FrameInputs& in) const {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = clear_for(in.gain);
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        ProbeUniforms u{};
        const float w = std::max(in.width, 1.0f);
        const float h = std::max(in.height, 1.0f);
        u.centre[0] = in.pointer_x / w * 2.0f - 1.0f;
        u.centre[1] = 1.0f - in.pointer_y / h * 2.0f;
        u.half_size[0] = 24.0f / w * 2.0f;
        u.half_size[1] = 24.0f / h * 2.0f;
        [enc setRenderPipelineState:pipeline_];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }

    /// One frame rendered offscreen at `pixels_w` x `pixels_h`, as PNG.
    bool render_png(std::uint32_t pixels_w, std::uint32_t pixels_h, const FrameInputs& in,
                    std::vector<std::uint8_t>& png) const {
        if (pixels_w == 0 || pixels_h == 0)
            return false;
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:pixels_w
                                                              height:pixels_h
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> texture = [device_ newTextureWithDescriptor:td];
        const NSUInteger row = pixels_w * 4;
        id<MTLBuffer> buffer = [device_ newBufferWithLength:row * pixels_h
                                                    options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        encode(cb, texture, in);
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(pixels_w, pixels_h, 1)
                            toBuffer:buffer
                   destinationOffset:0
              destinationBytesPerRow:row
            destinationBytesPerImage:row * pixels_h];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted)
            return false;

        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef provider =
            CGDataProviderCreateWithData(nullptr, buffer.contents, row * pixels_h, nullptr);
        CGImageRef image =
            CGImageCreate(pixels_w, pixels_h, 8, 32, row, space,
                          kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, provider,
                          nullptr, false, kCGRenderingIntentDefault);
        CFMutableDataRef data = CFDataCreateMutable(nullptr, 0);
        CGImageDestinationRef dest =
            CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, nullptr);
        bool ok = false;
        if (dest && image) {
            CGImageDestinationAddImage(dest, image, nullptr);
            ok = CGImageDestinationFinalize(dest);
        }
        if (ok) {
            const auto* bytes = CFDataGetBytePtr(data);
            png.assign(bytes, bytes + CFDataGetLength(data));
        }
        if (dest)
            CFRelease(dest);
        CFRelease(data);
        if (image)
            CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(space);
        return ok;
    }

  private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLRenderPipelineState> pipeline_ = nil;
};

class MetalProbeEditor final : public Editor, public EditorAutomation {
  public:
    explicit MetalProbeEditor(EditorContext& context) : context_(context) {}
    ~MetalProbeEditor() override {
        teardown(false);
    }

    bool supports_parent(NativeParentKind kind) const override {
        return kind == NativeParentKind::cocoa_view;
    }

    bool attach(const NativeParent& parent) override {
        if (parent.kind != NativeParentKind::cocoa_view || !parent.handle || surface_)
            return false;
        surface_ = MetalSurface::create();
        if (!surface_)
            return false;
        if (!renderer_.init((__bridge id<MTLDevice>)surface_->device(),
                            static_cast<MTLPixelFormat>(surface_->pixel_format()))) {
            surface_.reset();
            return false;
        }
        pump_ = std::make_unique<FramePump>([this](const FrameTick&) { render_frame(); },
                                            FramePumpOptions{60.0, false, FrameThread::render});
        surface_->set_release_handler([this] {
            if (pump_)
                pump_->stop();
        });
        InputHandler input;
        input.pointer = [this](const PointerEvent& e) { on_pointer(e); };
        surface_->set_input_handler(std::move(input));
        if (!surface_->attach(parent.handle)) {
            pump_.reset();
            surface_.reset();
            return false;
        }
        pump_->start();
        pump_->request_frame();
        return true;
    }

    void detach() override {
        teardown(true);
    }

    void invalidate() override {
        if (surface_)
            surface_->invalidate();
        teardown(false);
    }

    EditorSize size() const override {
        return size_;
    }

    bool set_size(const EditorSize& size) override {
        if (size.width == 0 || size.height == 0)
            return false;
        size_ = size;
        if (pump_)
            pump_->request_frame();
        return true;
    }

    ViewSize size_hints() const override {
        ViewSize hints;
        hints.preferred_width = 360;
        hints.preferred_height = 240;
        hints.min_width = 240;
        hints.min_height = 160;
        hints.max_width = 1440;
        hints.max_height = 960;
        return hints;
    }

    void idle() override {
        if (!surface_ || !pump_)
            return;
        surface_->poll_visibility();
        pump_->set_on_screen(surface_->on_screen().on_screen());
        // A host-side change (automation, a generic editor) repaints too.
        const float gain = context_.params().get_normalized(kGain);
        if (gain != last_gain_) {
            last_gain_ = gain;
            pump_->request_frame();
        }
    }

    EditorAutomation* automation() override {
        return this;
    }

    bool capture_png(std::vector<std::uint8_t>& png) override {
        if (!surface_)
            return false;
        const auto g = surface_->geometry();
        if (!g.in_window || g.drawable_width == 0)
            return false;
        return renderer_.render_png(g.drawable_width, g.drawable_height, inputs(), png);
    }

    /// Frames presented so far, for the host proof.
    std::uint64_t frames() const {
        return frames_.load();
    }

  private:
    FrameInputs inputs() const {
        FrameInputs in;
        // ParamPort::get is callable from any thread.
        in.gain = context_.params().get_normalized(kGain);
        in.pointer_x = pointer_x_.load();
        in.pointer_y = pointer_y_.load();
        const auto g = surface_ ? surface_->geometry() : SurfaceGeometry{};
        in.width = static_cast<float>(g.width);
        in.height = static_cast<float>(g.height);
        return in;
    }

    // Render thread.
    void render_frame() {
        @autoreleasepool {
            auto drawable = surface_->acquire_drawable();
            if (!drawable)
                return;
            id<MTLCommandBuffer> cb = [renderer_.queue() commandBuffer];
            renderer_.encode(cb, (__bridge id<MTLTexture>)drawable.texture(), inputs());
            drawable.present((__bridge void*)cb);
            [cb commit];
            frames_.fetch_add(1);
        }
    }

    // Main thread.
    void on_pointer(const PointerEvent& e) {
        pointer_x_.store(static_cast<float>(e.x));
        pointer_y_.store(static_cast<float>(e.y));
        auto& params = context_.params();
        const auto g = surface_->geometry();
        switch (e.kind) {
        case PointerKind::down:
            if (e.button == PointerButton::left && !dragging_) {
                dragging_ = true;
                drag_start_y_ = e.y;
                drag_start_gain_ = params.get_normalized(kGain);
                params.begin(kGain);
            }
            break;
        case PointerKind::drag:
            if (dragging_) {
                const double travel = std::max(g.height, 1.0);
                const double next = drag_start_gain_ + (drag_start_y_ - e.y) / travel;
                params.set_normalized(kGain, static_cast<float>(std::clamp(next, 0.0, 1.0)));
            }
            break;
        case PointerKind::up:
            if (dragging_ && e.button == PointerButton::left) {
                dragging_ = false;
                params.end(kGain);
            }
            break;
        default:
            break;
        }
        if (pump_)
            pump_->request_frame();
    }

    void teardown(bool orderly) {
        if (dragging_) {
            dragging_ = false;
            context_.params().end(kGain);
        }
        if (surface_ && orderly)
            surface_->detach(); // stops the pump through the release handler
        if (pump_)
            pump_->stop();
        pump_.reset();
        surface_.reset();
    }

    EditorContext& context_;
    EditorSize size_{360, 240, 1.0};
    std::unique_ptr<MetalSurface> surface_;
    std::unique_ptr<FramePump> pump_;
    ProbeRenderer renderer_;
    std::atomic<float> pointer_x_{180.0f};
    std::atomic<float> pointer_y_{120.0f};
    std::atomic<std::uint64_t> frames_{0};
    bool dragging_ = false;
    double drag_start_y_ = 0.0;
    double drag_start_gain_ = 0.0;
    float last_gain_ = -1.0f;
};

class MetalProbeFactory final : public EditorFactory {
  public:
    bool supports(EditorRole role) const override {
        return role == EditorRole::editor;
    }
    std::unique_ptr<Editor> create(Processor&, EditorContext& context, EditorRole role) override {
        if (role != EditorRole::editor)
            return nullptr;
        return std::make_unique<MetalProbeEditor>(context);
    }
};

} // namespace

::pulp::format::EditorFactory* editor_factory() {
    static MetalProbeFactory factory;
    return &factory;
}

} // namespace pulp_native_metal_probe
