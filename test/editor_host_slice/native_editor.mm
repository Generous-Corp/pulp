// The slice's bring-your-own editor: a plain AppKit view, no UI framework.
//
// It proves the editor-host contract alone is enough to ship an editor: the
// whole UI is one NSView that draws a gain bar, and everything it knows about
// the processor comes through the EditorContext (the ParamPort for the gain,
// the VisualPort for the meter). It defines this binary's
// `slice_editor_factory()`. Compiled with ARC.

#include "gain_meter_processor.hpp"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>

@interface PulpSliceNativeView : NSView
@property(nonatomic) float value;
@property(nonatomic) float meter;
@end

@implementation PulpSliceNativeView
- (BOOL)isFlipped {
    return YES;
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[NSColor colorWithCalibratedWhite:0.12 alpha:1.0] setFill];
    NSRectFill(self.bounds);
    const NSRect track = NSInsetRect(self.bounds, 12, self.bounds.size.height * 0.4);
    [[NSColor colorWithCalibratedWhite:0.3 alpha:1.0] setFill];
    NSRectFill(track);
    NSRect fill = track;
    fill.size.width *= self.value;
    [[NSColor colorWithCalibratedRed:0.2 green:0.7 blue:0.9 alpha:1.0] setFill];
    NSRectFill(fill);
    NSRect meter = NSMakeRect(12, 8, (self.bounds.size.width - 24) * self.meter, 4);
    [[NSColor colorWithCalibratedRed:0.4 green:0.9 blue:0.4 alpha:1.0] setFill];
    NSRectFill(meter);
}
@end

namespace pulp::slice {

namespace {

std::atomic<int> g_live{0};

constexpr std::uint32_t kWidth = 240;
constexpr std::uint32_t kHeight = 160;

class NativeEditor final : public format::Editor, public format::EditorAutomation {
  public:
    explicit NativeEditor(format::EditorContext& context) : context_(context) {
        ++g_live;
        view_ = [[PulpSliceNativeView alloc] initWithFrame:NSMakeRect(0, 0, kWidth, kHeight)];
        view_.value = context_.params().get_normalized(kGainParam);
        subscription_ = context_.params().add_listener([this](state::ParamID id, float) {
            if (id == kGainParam) {
                view_.value = context_.params().get_normalized(kGainParam);
                view_.needsDisplay = YES;
            }
        });
    }
    ~NativeEditor() override {
        detach();
        --g_live;
    }

    bool supports_parent(format::NativeParentKind kind) const override {
        return kind == format::NativeParentKind::cocoa_view ||
               kind == format::NativeParentKind::none;
    }
    bool attach(const format::NativeParent& parent) override {
        if (parent.kind == format::NativeParentKind::none)
            return true;
        if (parent.kind != format::NativeParentKind::cocoa_view || parent.handle == nullptr)
            return false;
        NSView* host = (__bridge NSView*)parent.handle;
        [host addSubview:view_];
        return true;
    }
    void detach() override {
        [view_ removeFromSuperview];
    }
    void invalidate() override {
        // The host's window is gone: forget it without touching it.
        view_ = [[PulpSliceNativeView alloc] initWithFrame:view_.frame];
    }
    format::EditorSize size() const override {
        return {static_cast<std::uint32_t>(view_.frame.size.width),
                static_cast<std::uint32_t>(view_.frame.size.height), scale_};
    }
    bool set_size(const format::EditorSize& size) override {
        if (size.width < 120 || size.height < 80)
            return false;
        [view_ setFrameSize:NSMakeSize(size.width, size.height)];
        scale_ = size.scale;
        return true;
    }
    void idle() override {
        if (auto* visuals = context_.visuals()) {
            std::array<float, 1> rms{}, peak{};
            if (visuals->read_meter(0, rms, peak) > 0)
                view_.meter = std::clamp(peak[0], 0.0f, 1.0f);
        }
        view_.needsDisplay = YES;
    }
    format::EditorAutomation* automation() override {
        return this;
    }

    // ── automation ──
    bool capture_png(std::vector<std::uint8_t>& png) override {
        NSBitmapImageRep* rep = [view_ bitmapImageRepForCachingDisplayInRect:view_.bounds];
        if (rep == nil)
            return false;
        [view_ cacheDisplayInRect:view_.bounds toBitmapImageRep:rep];
        NSData* data = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (data == nil)
            return false;
        png.assign(static_cast<const std::uint8_t*>(data.bytes),
                   static_cast<const std::uint8_t*>(data.bytes) + data.length);
        return true;
    }
    // "drag <dy>": a vertical drag, one gesture; 200 points span the range.
    bool inject_input(std::string_view request, std::string& error) override {
        if (request.rfind("drag ", 0) != 0) {
            error = "unsupported request";
            return false;
        }
        const float dy = std::strtof(std::string(request.substr(5)).c_str(), nullptr);
        auto& params = context_.params();
        const float start = params.get_normalized(kGainParam);
        const float target = std::clamp(start - dy / 200.0f, 0.0f, 1.0f);
        params.begin(kGainParam);
        constexpr int kSteps = 5;
        for (int i = 1; i <= kSteps; ++i)
            params.set_normalized(kGainParam, start + (target - start) * i / kSteps);
        params.end(kGainParam);
        return true;
    }
    bool observe(std::string_view, std::string& reply) override {
        reply = "{\"gain\":" + std::to_string(view_.value) + "}";
        return true;
    }

  private:
    format::EditorContext& context_;
    PulpSliceNativeView* view_ = nil;
    double scale_ = 1.0;
    format::ParamSubscription subscription_;
};

class NativeEditorFactory final : public format::EditorFactory {
  public:
    bool supports(format::EditorRole role) const override {
        return role == format::EditorRole::editor || role == format::EditorRole::inspector;
    }
    std::unique_ptr<format::Editor> create(format::Processor&, format::EditorContext& context,
                                           format::EditorRole role) override {
        if (!supports(role))
            return nullptr;
        return std::make_unique<NativeEditor>(context);
    }
};

} // namespace

format::EditorFactory* slice_editor_factory() {
    static NativeEditorFactory factory;
    return &factory;
}

int slice_editor_live_instances() {
    return g_live.load();
}

} // namespace pulp::slice
