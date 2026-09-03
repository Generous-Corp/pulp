// Opening a document on macOS has two halves, and each is useless alone.
//
// The bundle must DECLARE the type (CFBundleDocumentTypes, emitted by
// pulp_declare_standalone_document_type() — covered by
// test_standalone_document_types.cmake), or Launch Services never routes the
// file to this app at all. And the app must HEAR about it at runtime, which is
// what this file pins: an NSApplication delegate that exists before the event
// loop starts, converts the URLs it is handed, and does not lose a file that
// arrived before anyone was listening.
//
// The launch-time open is the case that motivates all of it. It is reported
// from inside [NSApp run], so anything wired up after the loop starts misses
// precisely the file the user double-clicked to launch the app.

#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#import <Cocoa/Cocoa.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

std::unique_ptr<pulp::view::WindowHost> make_hidden_host(pulp::view::View& root) {
    (void)[NSApplication sharedApplication];  // NSWindow needs a shared app
    pulp::view::WindowOptions opts;
    opts.width = 320;
    opts.height = 240;
    opts.title = "Pulp open-document host";
    opts.initially_hidden = true;
    opts.use_gpu = false;
    return pulp::view::WindowHost::create(root, opts);
}

// Send the message the OS sends. Nothing else in a unit test can produce the
// Apple Event, so the delegate is driven directly — which still exercises the
// real installed object, its URL conversion, and the queue behind it.
void send_open_urls(NSArray<NSURL*>* urls) {
    id<NSApplicationDelegate> delegate = [NSApplication sharedApplication].delegate;
    REQUIRE(delegate != nil);
    REQUIRE([delegate respondsToSelector:@selector(application:openURLs:)]);
    [delegate application:[NSApplication sharedApplication] openURLs:urls];
}

}  // namespace

TEST_CASE("macOS open-document seam delivers, filters, and never drops a file",
          "[view][mac][open-document]") {
    pulp::view::View root;
    auto host = make_hidden_host(root);
    REQUIRE(host != nullptr);

    std::vector<std::string> received;
    host->set_open_files_handler([&](const std::vector<std::string>& paths) {
        received.insert(received.end(), paths.begin(), paths.end());
    });

    // Installing a handler is what claims the NSApplication delegate. Without
    // this the rest of the test would be driving nothing, so assert it directly
    // rather than inferring it from a later delivery.
    REQUIRE([NSApplication sharedApplication].delegate != nil);

    SECTION("a file URL arrives as a filesystem path") {
        send_open_urls(@[ [NSURL fileURLWithPath:@"/tmp/pulp-open-doc.forge"] ]);
        REQUIRE(received.size() == 1);
        CHECK(received[0] == "/tmp/pulp-open-doc.forge");
    }

    SECTION("several files in one open arrive together, in order") {
        send_open_urls(@[ [NSURL fileURLWithPath:@"/tmp/a.forge"],
                          [NSURL fileURLWithPath:@"/tmp/b.forge"] ]);
        REQUIRE(received.size() == 2);
        CHECK(received[0] == "/tmp/a.forge");
        CHECK(received[1] == "/tmp/b.forge");
    }

    SECTION("a non-file URL is dropped rather than handed over as a path") {
        // Pulp declares no CFBundleURLTypes, so this cannot be routed here
        // today; passing "https://example.com/x" to a handler that expects a
        // path would hand it something it cannot open.
        send_open_urls(@[ [NSURL URLWithString:@"https://example.com/x"],
                          [NSURL fileURLWithPath:@"/tmp/kept.forge"] ]);
        REQUIRE(received.size() == 1);
        CHECK(received[0] == "/tmp/kept.forge");
    }

    SECTION("a file that arrives with no handler installed is held, not lost") {
        // The launch-time case: the OS reports the file before the app has
        // wired anything up. Removing the handler reproduces that window
        // without needing a second process.
        host->set_open_files_handler({});
        send_open_urls(@[ [NSURL fileURLWithPath:@"/tmp/launched-with.forge"] ]);
        CHECK(received.empty());

        std::vector<std::string> late;
        host->set_open_files_handler([&](const std::vector<std::string>& paths) {
            late.insert(late.end(), paths.begin(), paths.end());
        });
        REQUIRE(late.size() == 1);
        CHECK(late[0] == "/tmp/launched-with.forge");

        // And the queue is emptied by that flush: installing another handler
        // must not replay a file the app has already opened.
        std::vector<std::string> again;
        host->set_open_files_handler([&](const std::vector<std::string>& paths) {
            again.insert(again.end(), paths.begin(), paths.end());
        });
        CHECK(again.empty());
    }

    host->set_open_files_handler({});
}

TEST_CASE("the app delegate never displaces one that is already installed",
          "[view][mac][open-document]") {
    // A Pulp plug-in loaded into a DAW shares that host's NSApplication. Taking
    // over its delegate would break the host's own document, reopen, and
    // termination messages — a far worse failure than not receiving opens,
    // which the host owns anyway.
    (void)[NSApplication sharedApplication];
    id previous = [NSApplication sharedApplication].delegate;

    NSObject* foreign = [[NSObject alloc] init];
    [NSApplication sharedApplication].delegate = (id<NSApplicationDelegate>)foreign;

    pulp::view::View root;
    auto host = make_hidden_host(root);
    REQUIRE(host != nullptr);
    host->set_open_files_handler([](const std::vector<std::string>&) {});

    CHECK([NSApplication sharedApplication].delegate == (id)foreign);

    [NSApplication sharedApplication].delegate = previous;
}
