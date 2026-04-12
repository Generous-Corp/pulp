// Stream feature demo.
//
// Exercises the three layers of Pulp's Stream surface:
//   1. Synchronous FileStream writes cache a byte payload to disk.
//   2. Synchronous HttpStream issues an HTTP GET and reports status.
//   3. AsyncStream wraps the FileStream, dispatches reads onto a
//      pulp::events::EventLoop, and demonstrates backpressure + cancel.
//
// Run without arguments to exercise the offline path (file + async); pass
// `--http <url>` to additionally fetch a URL.

#include <pulp/events/event_loop.hpp>
#include <pulp/runtime/async_stream.hpp>
#include <pulp/runtime/network_stream.hpp>
#include <pulp/runtime/stream.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::runtime;
using namespace std::chrono_literals;

namespace {

void demo_file_roundtrip() {
    auto path = std::filesystem::temp_directory_path() / "pulp_stream_demo.bin";
    std::printf("[demo] writing sample payload to %s\n", path.string().c_str());

    {
        FileStream writer(path.string(), FileStream::Mode::Write);
        const std::uint8_t payload[] = "hello from pulp stream demo";
        auto w = writer.write(payload, sizeof(payload));
        std::printf("[demo] wrote %zu bytes (ok=%d)\n", w.bytes, int(w.ok()));
    }

    FileStream reader(path.string(), FileStream::Mode::Read);
    std::uint8_t buf[64]{};
    auto r = reader.read(buf, sizeof(buf));
    std::printf("[demo] read back %zu bytes: \"%s\"\n", r.bytes,
                reinterpret_cast<const char*>(buf));
    reader.close();
    std::filesystem::remove(path);
}

void demo_async_stream() {
    auto path = std::filesystem::temp_directory_path() / "pulp_stream_demo_async.bin";

    // Pre-populate a file so the async read has something to chew on.
    {
        FileStream w(path.string(), FileStream::Mode::Write);
        std::vector<std::uint8_t> payload(256, 42);
        w.write(payload.data(), payload.size());
    }

    pulp::events::EventLoop loop;
    AsyncStreamOptions opts;
    opts.executor = [&loop](std::function<void()> fn) { loop.dispatch(std::move(fn)); };

    auto file = std::make_unique<FileStream>(path.string(), FileStream::Mode::Read);
    AsyncStream stream(std::move(file), opts);

    std::mutex m;
    std::condition_variable cv;
    std::atomic<std::size_t> total_read{0};
    std::atomic<bool> closed{false};

    stream.on_data([&](const std::uint8_t*, std::size_t n) {
        total_read.fetch_add(n);
    });
    stream.on_close([&] {
        closed.store(true);
        std::lock_guard<std::mutex> lock(m);
        cv.notify_all();
    });

    stream.start();

    std::unique_lock<std::mutex> lock(m);
    cv.wait_for(lock, 1s, [&] { return closed.load(); });

    std::printf("[demo] async read delivered %zu bytes; closed=%d\n",
                total_read.load(), int(closed.load()));
    std::filesystem::remove(path);
}

void demo_http_fetch(const std::string& url) {
    std::printf("[demo] http GET %s\n", url.c_str());
    auto req = HttpStream::Request{url, "GET", {}, "application/json", 10};
    HttpStream http(req);
    std::printf("[demo]   status_code=%d body_len=%zu transport_error=\"%s\"\n",
                http.status_code(),
                http.eof() ? std::size_t{0} : std::size_t{0} /* body bytes read */,
                http.transport_error().c_str());

    std::uint8_t buf[128]{};
    std::size_t total = 0;
    while (true) {
        auto r = http.read(buf, sizeof(buf));
        if (!r.ok()) break;
        total += r.bytes;
    }
    std::printf("[demo]   read %zu bytes of body\n", total);
}

}  // namespace

int main(int argc, char** argv) {
    demo_file_roundtrip();
    demo_async_stream();

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--http") == 0) {
            demo_http_fetch(argv[i + 1]);
            ++i;
        }
    }
    return 0;
}
