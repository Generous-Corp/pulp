// Cross-process hover-cursor probe.
//
// Answers one question about a running application: does moving the pointer
// with NO button held change the cursor a person actually sees?
//
// The oracle is +[NSCursor currentSystemCursor], read from OUTSIDE the target
// process, so it reports the cursor the window server is displaying rather than
// whatever the target's own resolver computed. A target can compute the right
// answer and still show the arrow; only the system cursor distinguishes those.
//
// Method: decouple the HID mouse from the cursor so synthetic positioning is
// authoritative, warp across a grid inside the target window, and hash the
// system cursor image at each sample. A surface with hover feedback yields more
// than one distinct cursor. A surface where hover never changes the cursor
// yields exactly one, which is the defect this probe exists to catch.
//
// The button-held comparison is deliberate: a build that changes the cursor
// only while a button is down reads as "no hover feedback, but feedback on
// drag", which is a different and much more specific finding than "no cursor
// code at all".

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <CommonCrypto/CommonDigest.h>
#include <getopt.h>
#include <signal.h>
#include <math.h>

static void reassociate(void) { CGAssociateMouseAndMouseCursorPosition(true); }

// The pointer is decoupled from the hardware mouse during a sweep. If this
// process dies without re-associating, the machine is left with a mouse that
// moves nothing and the user has to log out. Restoration is therefore wired to
// every exit path this process can take, not just the normal one.
static void fatal_signal(int sig) { reassociate(); _exit(128 + sig); }
static void install_teardown(void) {
    atexit(reassociate);
    int sigs[] = {SIGINT, SIGTERM, SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGQUIT, SIGHUP, SIGPIPE};
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) signal(sigs[i], fatal_signal);
}

// A human touching the trackpad while a sweep runs overrides the synthetic
// position, so the sample is read at a point nobody chose. That corrupts a
// verdict in BOTH directions -- a missed event reads as "cursor never changed",
// and a real pointer resting over a live region reads as a change that the
// probe did not cause. Neither is detectable in the cursor value itself, so
// contention is detected positionally and voids the run instead of averaging
// through it.
static const CGFloat kContendedSlopPx = 2.0;
static BOOL pointerIsWhereWePutIt(CGPoint want) {
    CGEventRef e = CGEventCreate(NULL);
    CGPoint got = CGEventGetLocation(e);
    CFRelease(e);
    return (fabs(got.x - want.x) <= kContendedSlopPx && fabs(got.y - want.y) <= kContendedSlopPx);
}

// Identity of the cursor currently on screen. Hashing the image bytes rather
// than comparing NSCursor pointers is what makes this work across processes:
// the singletons in this process are not the ones the target set.
// A screenshot cannot serve as the visual record here: macOS does not
// composite the pointer into a -R region capture (verified), so the cursor is
// absent from any frame grab. The cursor's own image IS the artifact, so it is
// written out directly and a human can look at what was on screen.
static NSString* g_dumpDir = nil;
static void dumpCursorImage(NSString* key) {
    if (!g_dumpDir) return;
    NSCursor* sc = [NSCursor currentSystemCursor];
    if (!sc || !sc.image) return;
    NSString* path = [g_dumpDir stringByAppendingPathComponent:
        [NSString stringWithFormat:@"cursor-%@.png", [key stringByReplacingOccurrencesOfString:@"/" withString:@"_"]]];
    if ([[NSFileManager defaultManager] fileExistsAtPath:path]) return;
    CGImageRef cg = [sc.image CGImageForProposedRect:NULL context:nil hints:nil];
    if (!cg) return;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:path atomically:YES];
}

static NSString* systemCursorKey(void) {
    NSCursor* sc = [NSCursor currentSystemCursor];
    if (sc == nil) return nil;
    NSData* tiff = [[sc image] TIFFRepresentation];
    if (tiff == nil) return nil;
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256([tiff bytes], (CC_LONG)[tiff length], md);
    NSMutableString* s = [NSMutableString string];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", md[i]];
    return [NSString stringWithFormat:@"%@/%.0fx%.0f", [s substringToIndex:8],
                     sc.image.size.width, sc.image.size.height];
}

// Frontmost on-screen window belonging to `pid`, in CG (top-left origin) coords.
static BOOL windowRectForPid(pid_t pid, CGRect* out) {
    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return NO;
    BOOL found = NO;
    CGRect best = CGRectZero;
    for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
        NSDictionary* w = (__bridge NSDictionary*)CFArrayGetValueAtIndex(list, i);
        if ([w[(id)kCGWindowOwnerPID] intValue] != pid) continue;
        CGRect r;
        if (!CGRectMakeWithDictionaryRepresentation(
                (__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &r)) continue;
        if (r.size.width < 80 || r.size.height < 80) continue;  // skip shadows/panels
        if (!found || r.size.width * r.size.height > best.size.width * best.size.height) {
            best = r; found = YES;
        }
    }
    CFRelease(list);
    if (found) *out = best;
    return found;
}

static void settle(double secs) {
    NSDate* end = [NSDate dateWithTimeIntervalSinceNow:secs];
    while ([end timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}

// Returns nil and sets *contended when a human moved the pointer during the
// sample.
static NSString* sampleAt(CGPoint p, double dwell, BOOL* contended) {
    CGWarpMouseCursorPosition(p);
    // A warp alone repositions without telling the app; the moved event is what
    // drives its tracking area. No button is ever pressed.
    CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, kCGMouseButtonLeft);
    CGEventPost(kCGSessionEventTap, mv);
    CFRelease(mv);
    settle(dwell);
    if (!pointerIsWhereWePutIt(p)) { *contended = YES; return nil; }
    NSString* k = systemCursorKey();
    // Re-check after the read: a move that lands mid-read is just as corrupting.
    if (!pointerIsWhereWePutIt(p)) { *contended = YES; return nil; }
    return k;
}

int main(int argc, char** argv) { @autoreleasepool {
    pid_t pid = 0; int cols = 6, rows = 4; double dwell = 0.20; int withDrag = 0;
    NSString* label = @"target";
    static struct option opts[] = {
        {"pid", required_argument, 0, 'p'}, {"cols", required_argument, 0, 'c'},
        {"rows", required_argument, 0, 'r'}, {"dwell", required_argument, 0, 'd'},
        {"label", required_argument, 0, 'l'}, {"compare-drag", no_argument, 0, 'D'},
        {"dump-cursors", required_argument, 0, 'O'}, {0,0,0,0}};
    int ch;
    while ((ch = getopt_long(argc, argv, "p:c:r:d:l:DO:", opts, NULL)) != -1) {
        switch (ch) {
            case 'p': pid = (pid_t)atoi(optarg); break;
            case 'c': cols = atoi(optarg); break;
            case 'r': rows = atoi(optarg); break;
            case 'd': dwell = atof(optarg); break;
            case 'l': label = @(optarg); break;
            case 'D': withDrag = 1; break;
            case 'O': g_dumpDir = @(optarg);
                [[NSFileManager defaultManager] createDirectoryAtPath:g_dumpDir
                    withIntermediateDirectories:YES attributes:nil error:nil]; break;
            default: fprintf(stderr, "usage: %s --pid N [--cols N --rows N --dwell S --label X --compare-drag]\n", argv[0]); return 2;
        }
    }
    if (pid <= 0) { fprintf(stderr, "ERROR: --pid is required\n"); return 2; }
    [NSApplication sharedApplication];

    if (!AXIsProcessTrusted()) {
        fprintf(stderr, "ERROR: this process is not trusted for Accessibility; "
                        "synthetic pointer events will not be delivered.\n");
        return 3;
    }

    CGRect win;
    if (!windowRectForPid(pid, &win)) {
        fprintf(stderr, "ERROR: no on-screen window found for pid %d\n", pid);
        return 4;
    }
    printf("probe: %s pid=%d window=%.0f,%.0f %.0fx%.0f grid=%dx%d dwell=%.2fs\n",
           [label UTF8String], pid, win.origin.x, win.origin.y,
           win.size.width, win.size.height, cols, rows, dwell);

    install_teardown();
    CGAssociateMouseAndMouseCursorPosition(false);

    // Inset so samples land on content rather than the frame edge / title bar.
    const CGFloat inx = win.size.width * 0.10, iny = win.size.height * 0.10;
    const CGFloat x0 = win.origin.x + inx, y0 = win.origin.y + iny;
    const CGFloat w = win.size.width - 2 * inx, h = win.size.height - 2 * iny;

    NSMutableDictionary<NSString*, NSNumber*>* hist = [NSMutableDictionary dictionary];
    NSMutableArray* grid = [NSMutableArray array];
    for (int r = 0; r < rows; r++) {
        NSMutableString* line = [NSMutableString string];
        for (int c = 0; c < cols; c++) {
            CGPoint p = CGPointMake(x0 + w * (c + 0.5) / cols, y0 + h * (r + 0.5) / rows);
            BOOL contended = NO;
            NSString* k = sampleAt(p, dwell, &contended);
            if (contended) {
                fprintf(stderr, "\nVOID: human input detected during the sweep "
                                "(pointer moved off the sampled point). Result discarded.\n"
                                "      Re-run on an idle machine.\n");
                return 6;
            }
            if (k == nil) k = @"<unreadable>"; else dumpCursorImage(k);
            hist[k] = @([hist[k] intValue] + 1);
            [line appendFormat:@"%@ ", [k substringToIndex:4]];
        }
        [grid addObject:line];
    }

    printf("\nhover sweep (no button held) — cursor id per sample:\n");
    for (NSString* l in grid) printf("  %s\n", [l UTF8String]);
    printf("\ndistinct cursors while hovering: %lu\n", (unsigned long)hist.count);
    for (NSString* k in hist) printf("   %-20s x%d\n", [k UTF8String], [hist[k] intValue]);

    int hoverDistinct = (int)hist.count;
    int dragDistinct = -1;
    if (withDrag) {
        // Same sweep with the left button held. If this yields more distinct
        // cursors than hovering did, the target has cursor logic that only runs
        // on button-driven events — the reported symptom.
        NSMutableDictionary* dhist = [NSMutableDictionary dictionary];
        CGPoint start = CGPointMake(x0 + w * 0.5, y0 + h * 0.5);
        CGWarpMouseCursorPosition(start);
        CGEventRef down = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDown, start, kCGMouseButtonLeft);
        CGEventPost(kCGSessionEventTap, down); CFRelease(down);
        settle(0.15);
        for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
            CGPoint p = CGPointMake(x0 + w * (c + 0.5) / cols, y0 + h * (r + 0.5) / rows);
            CGWarpMouseCursorPosition(p);
            CGEventRef dr = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDragged, p, kCGMouseButtonLeft);
            CGEventPost(kCGSessionEventTap, dr); CFRelease(dr);
            settle(dwell);
            if (!pointerIsWhereWePutIt(p)) {
                CGEventRef u = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp, p, kCGMouseButtonLeft);
                CGEventPost(kCGSessionEventTap, u); CFRelease(u);
                fprintf(stderr, "\nVOID: human input detected during the button-held sweep. "
                                "Result discarded.\n");
                return 6;
            }
            NSString* k = systemCursorKey(); if (!k) k = @"<unreadable>";
            dhist[k] = @([dhist[k] intValue] + 1);
        }
        CGEventRef up = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp,
            CGPointMake(x0 + w * 0.5, y0 + h * 0.5), kCGMouseButtonLeft);
        CGEventPost(kCGSessionEventTap, up); CFRelease(up);
        settle(0.15);
        dragDistinct = (int)[dhist count];
        printf("\ndistinct cursors while a button IS held: %d\n", dragDistinct);
        for (NSString* k in dhist) printf("   %-20s x%d\n", [k UTF8String], [dhist[k] intValue]);
    }

    printf("\n== VERDICT (%s) ==\n", [label UTF8String]);
    if (hoverDistinct >= 2) {
        printf("PASS: hovering with no button held changes the cursor (%d distinct).\n", hoverDistinct);
        return 0;
    }
    if (hoverDistinct <= 0) { printf("INCONCLUSIVE: cursor was unreadable.\n"); return 5; }
    printf("FAIL: the cursor never changed while hovering (%d distinct across %d samples).\n",
           hoverDistinct, cols * rows);
    if (dragDistinct > hoverDistinct)
        printf("      and it DID change with a button held (%d distinct) — "
               "cursor feedback is reaching only button-driven events.\n", dragDistinct);
    return 1;
}}
