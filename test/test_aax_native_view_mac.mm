// Native frame invariants for the AAX editor shell's child placement, driven
// against real NSViews with no window: the parent Pro Tools hands down may be
// any size, and the child must land at its top-left (not AppKit's default
// bottom-left origin) or fill it on request. Also pins the no-op case that
// keeps VST3/CLAP unchanged: a parent built at the child's size never moves.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/aax_native_view.hpp>

#import <AppKit/AppKit.h>

using namespace pulp::format::aax;

@interface PulpAaxFlippedParent : NSView
@end
@implementation PulpAaxFlippedParent
- (BOOL)isFlipped { return YES; }
@end

namespace {

struct Fixture {
    NSView* parent;
    NSView* child;
    Fixture(NSSize parent_size, NSSize child_size, bool flipped = false) {
        const NSRect pr = NSMakeRect(0, 0, parent_size.width, parent_size.height);
        parent = flipped ? [[PulpAaxFlippedParent alloc] initWithFrame:pr]
                         : [[NSView alloc] initWithFrame:pr];
        child = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, child_size.width,
                                                         child_size.height)];
        child.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [parent addSubview:child];
    }
    void* p() const { return (__bridge void*)parent; }
    void* c() const { return (__bridge void*)child; }
};

} // namespace

TEST_CASE("AAX child top-left origin is the parent height minus the child height",
          "[aax][view-bridge][resize]") {
    CHECK(aax_child_top_left_y(1400.0f, 645.0f, false) == 755.0f);
    CHECK(aax_child_top_left_y(645.0f, 645.0f, false) == 0.0f);
    // A parent smaller than the child cannot be lifted above its own origin.
    CHECK(aax_child_top_left_y(500.0f, 645.0f, false) == 0.0f);
    // A flipped parent already has (0,0) at the top-left.
    CHECK(aax_child_top_left_y(1400.0f, 645.0f, true) == 0.0f);
}

TEST_CASE("AAX native child moves to the top-left of a larger host parent",
          "[aax][view-bridge][resize]") {
    @autoreleasepool {
        Fixture f(NSMakeSize(2000, 1400), NSMakeSize(990, 645));
        // AppKit's default: a subview added at (0,0) sits bottom-left.
        REQUIRE(f.child.frame.origin.y == 0);

        aax_place_child_top_left(f.p(), f.c());

        CHECK(f.child.frame.origin.x == 0);
        CHECK(f.child.frame.origin.y == Catch::Approx(1400 - 645));
        CHECK(f.child.frame.size.width == 990);
        CHECK(f.child.frame.size.height == 645);

        const auto g = aax_native_geometry(f.p(), f.c());
        CHECK(g.valid);
        CHECK_FALSE(g.has_window);
        CHECK_FALSE(g.parent_flipped);
        CHECK(g.parent_bounds.width == 2000);
        CHECK(g.parent_bounds.height == 1400);
        CHECK(g.child_frame.y == Catch::Approx(755));
        CHECK(aax_describe_geometry(g).find("child=990x645@0,755") != std::string::npos);
    }
}

TEST_CASE("AAX native child stays put when the host parent matches its size",
          "[aax][view-bridge][resize]") {
    // The VST3/CLAP shape: the host builds the parent at the requested size.
    // Placement must be a no-op there so those adapters' frames never change.
    @autoreleasepool {
        Fixture f(NSMakeSize(990, 645), NSMakeSize(990, 645));
        aax_place_child_top_left(f.p(), f.c());
        CHECK(NSEqualRects(f.child.frame, NSMakeRect(0, 0, 990, 645)));
        aax_fill_parent(f.p(), f.c());
        CHECK(NSEqualRects(f.child.frame, NSMakeRect(0, 0, 990, 645)));
    }
}

TEST_CASE("AAX native child fills an off-size host parent on request",
          "[aax][view-bridge][resize]") {
    @autoreleasepool {
        Fixture f(NSMakeSize(1980, 1290), NSMakeSize(990, 645));
        aax_fill_parent(f.p(), f.c());
        CHECK(NSEqualRects(f.child.frame, NSMakeRect(0, 0, 1980, 1290)));
        // Once it fills, the flexible autoresizing mask tracks the parent 1:1.
        [f.parent setFrameSize:NSMakeSize(990, 645)];
        CHECK(NSEqualRects(f.child.frame, NSMakeRect(0, 0, 990, 645)));
    }
}

TEST_CASE("AAX native child must not track a host parent that grows from 0x0",
          "[aax][view-bridge][resize]") {
    // Pro Tools hands over a parent it has not laid out yet and sizes it from
    // the plug-in's SetViewSize. AppKit autoresizing applies the parent's
    // DELTA, so a 990x645 child that tracks a 0x0 parent becomes 1980x1290
    // the moment the parent reaches 990x645 — the doubled frame the editor
    // would then publish back to the host.
    @autoreleasepool {
        Fixture hazard(NSMakeSize(0, 0), NSMakeSize(990, 645));
        [hazard.parent setFrameSize:NSMakeSize(990, 645)];
        // Positive control: the hazard is real with tracking left on.
        CHECK(hazard.child.frame.size.width == 1980);
        CHECK(hazard.child.frame.size.height == 1290);

        Fixture guarded(NSMakeSize(0, 0), NSMakeSize(990, 645));
        aax_set_child_tracks_parent(guarded.c(), false);
        [guarded.parent setFrameSize:NSMakeSize(990, 645)];
        CHECK(guarded.child.frame.size.width == 990);
        CHECK(guarded.child.frame.size.height == 645);
        // Tracking restored once parent and child agree: later host resizes
        // follow 1:1 again.
        aax_set_child_tracks_parent(guarded.c(), true);
        [guarded.parent setFrameSize:NSMakeSize(1138, 741)];
        CHECK(guarded.child.frame.size.width == 1138);
        CHECK(guarded.child.frame.size.height == 741);
    }
}

TEST_CASE("AAX native child placement honours a flipped host parent",
          "[aax][view-bridge][resize]") {
    @autoreleasepool {
        Fixture f(NSMakeSize(2000, 1400), NSMakeSize(990, 645), /*flipped=*/true);
        aax_place_child_top_left(f.p(), f.c());
        CHECK(f.child.frame.origin.y == 0);
        CHECK(aax_native_geometry(f.p(), f.c()).parent_flipped);
    }
}

TEST_CASE("AAX native geometry tolerates missing views", "[aax][view-bridge]") {
    const auto g = aax_native_geometry(nullptr, nullptr);
    CHECK_FALSE(g.valid);
    CHECK(aax_describe_geometry(g) == "invalid");
    aax_place_child_top_left(nullptr, nullptr);
    aax_fill_parent(nullptr, nullptr);
}
