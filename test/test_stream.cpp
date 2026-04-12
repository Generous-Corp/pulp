#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/stream.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

using pulp::runtime::FileStream;
using pulp::runtime::MemoryStream;
using pulp::runtime::Stream;
using pulp::runtime::StreamError;
using pulp::runtime::StreamResult;

namespace {

std::filesystem::path make_temp_path(const char* stem) {
    auto dir = std::filesystem::temp_directory_path();
    auto name = std::string(stem) + "_" + std::to_string(std::rand()) + ".bin";
    return dir / name;
}

}  // namespace

TEST_CASE("MemoryStream round-trip", "[stream]") {
    MemoryStream s;
    const std::uint8_t msg[] = {1, 2, 3, 4, 5};
    auto w = s.write(msg, sizeof(msg));
    REQUIRE(w.ok());
    REQUIRE(w.bytes == sizeof(msg));
    REQUIRE(s.size() == sizeof(msg));

    std::uint8_t out[5]{};
    auto r = s.read(out, sizeof(out));
    REQUIRE(r.ok());
    REQUIRE(r.bytes == sizeof(msg));
    REQUIRE(std::memcmp(out, msg, sizeof(msg)) == 0);

    // Reading past the end reports Closed / EOF.
    auto eof = s.read(out, sizeof(out));
    REQUIRE_FALSE(eof.ok());
    REQUIRE(eof.closed());
}

TEST_CASE("MemoryStream partial read", "[stream]") {
    MemoryStream s(std::vector<std::uint8_t>{10, 20, 30, 40});
    std::uint8_t out[2]{};

    auto r1 = s.read(out, 2);
    REQUIRE(r1.ok());
    REQUIRE(r1.bytes == 2);
    REQUIRE(out[0] == 10);
    REQUIRE(out[1] == 20);

    auto r2 = s.read(out, 5);
    REQUIRE(r2.ok());
    REQUIRE(r2.bytes == 2);
    REQUIRE(out[0] == 30);
    REQUIRE(out[1] == 40);
}

TEST_CASE("MemoryStream close rejects further I/O", "[stream]") {
    MemoryStream s;
    s.close();
    REQUIRE_FALSE(s.is_open());

    std::uint8_t buf[4]{};
    auto r = s.read(buf, 4);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.closed());

    auto w = s.write(buf, 4);
    REQUIRE_FALSE(w.ok());
    REQUIRE(w.closed());
}

TEST_CASE("FileStream round-trip via filesystem", "[stream]") {
    auto path = make_temp_path("pulp_stream");
    const std::uint8_t payload[] = {'p', 'u', 'l', 'p', 0, 1, 2, 3};

    {
        FileStream w(path.string(), FileStream::Mode::Write);
        REQUIRE(w.is_open());
        auto r = w.write(payload, sizeof(payload));
        REQUIRE(r.ok());
        REQUIRE(r.bytes == sizeof(payload));
        REQUIRE(w.flush());
    }

    {
        FileStream r(path.string(), FileStream::Mode::Read);
        REQUIRE(r.is_open());
        std::uint8_t out[sizeof(payload)]{};
        auto got = r.read(out, sizeof(out));
        REQUIRE(got.ok());
        REQUIRE(got.bytes == sizeof(payload));
        REQUIRE(std::memcmp(out, payload, sizeof(payload)) == 0);

        // Next read reports end-of-stream.
        std::uint8_t tail[4]{};
        auto eof = r.read(tail, sizeof(tail));
        REQUIRE_FALSE(eof.ok());
        REQUIRE(eof.closed());
    }

    std::filesystem::remove(path);
}

TEST_CASE("FileStream open failure leaves stream closed", "[stream]") {
    FileStream s;
    REQUIRE_FALSE(s.open("/definitely/not/a/real/path/pulp_stream_test.bin",
                         FileStream::Mode::Read));
    REQUIRE_FALSE(s.is_open());
    std::uint8_t buf[2]{};
    auto r = s.read(buf, sizeof(buf));
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.closed());
}

TEST_CASE("Stream polymorphic usage", "[stream]") {
    MemoryStream backing;
    Stream& s = backing;

    const char* msg = "hello stream";
    auto w = s.write(reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg));
    REQUIRE(w.ok());
    REQUIRE(w.bytes == std::strlen(msg));

    std::array<std::uint8_t, 32> buf{};
    auto r = s.read(buf.data(), buf.size());
    REQUIRE(r.ok());
    REQUIRE(r.bytes == std::strlen(msg));
    REQUIRE(std::memcmp(buf.data(), msg, std::strlen(msg)) == 0);
}
