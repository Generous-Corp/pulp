#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/named_pipe.hpp>
#include <pulp/runtime/network_stream.hpp>
#include <pulp/runtime/stream.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

using pulp::runtime::FileStream;
using pulp::runtime::HttpStream;
using pulp::runtime::MemoryStream;
using pulp::runtime::NamedPipe;
using pulp::runtime::PipeStream;
using pulp::runtime::Stream;
using pulp::runtime::StreamError;
using pulp::runtime::StreamResult;
using pulp::runtime::TcpStream;

namespace {

std::filesystem::path make_temp_path(const char* stem) {
    auto dir = std::filesystem::temp_directory_path();
    auto name = std::string(stem) + "_" + std::to_string(std::rand()) + ".bin";
    return dir / name;
}

}  // namespace

TEST_CASE("StreamResult helper predicates classify errors",
          "[stream][coverage][phase3]") {
    auto ok = StreamResult::make(3);
    REQUIRE(ok.ok());
    REQUIRE_FALSE(ok.would_block());
    REQUIRE_FALSE(ok.closed());
    REQUIRE(ok.bytes == 3);

    auto would_block = StreamResult::fail(StreamError::WouldBlock);
    REQUIRE_FALSE(would_block.ok());
    REQUIRE(would_block.would_block());
    REQUIRE_FALSE(would_block.closed());

    auto invalid = StreamResult::fail(StreamError::Invalid);
    REQUIRE_FALSE(invalid.ok());
    REQUIRE_FALSE(invalid.would_block());
    REQUIRE_FALSE(invalid.closed());
}

TEST_CASE("StreamResult factory preserves explicit zero-byte success",
          "[stream][coverage][phase3]") {
    auto result = StreamResult::make(0);
    REQUIRE(result.ok());
    REQUIRE_FALSE(result.would_block());
    REQUIRE_FALSE(result.closed());
    REQUIRE(result.bytes == 0);
}

TEST_CASE("StreamResult closed failure has no transferred bytes",
          "[stream][coverage][phase3]") {
    auto result = StreamResult::fail(StreamError::Closed);
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.would_block());
    REQUIRE(result.closed());
    REQUIRE(result.bytes == 0);
}

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

TEST_CASE("MemoryStream zero-size, rewind, and clear edge paths", "[stream]") {
    MemoryStream s(std::vector<std::uint8_t>{7, 8, 9});
    std::uint8_t out[3]{};

    auto zero_read = s.read(out, 0);
    REQUIRE(zero_read.ok());
    REQUIRE(zero_read.bytes == 0);
    REQUIRE(s.read_position() == 0);

    auto first = s.read(out, 2);
    REQUIRE(first.ok());
    REQUIRE(first.bytes == 2);
    REQUIRE(s.read_position() == 2);

    s.rewind();
    REQUIRE(s.read_position() == 0);
    auto replay = s.read(out, sizeof(out));
    REQUIRE(replay.ok());
    REQUIRE(replay.bytes == 3);
    REQUIRE(out[0] == 7);
    REQUIRE(out[1] == 8);
    REQUIRE(out[2] == 9);

    const std::uint8_t payload[] = {1, 2};
    auto zero_write = s.write(payload, 0);
    REQUIRE(zero_write.ok());
    REQUIRE(zero_write.bytes == 0);
    REQUIRE(s.size() == 3);

    s.clear();
    REQUIRE(s.is_open());
    REQUIRE(s.size() == 0);
    REQUIRE(s.read_position() == 0);
    auto eof = s.read(out, sizeof(out));
    REQUIRE_FALSE(eof.ok());
    REQUIRE(eof.closed());
}

TEST_CASE("StreamResult helpers classify non-ok states", "[stream][coverage][issue-656]") {
    auto ok = StreamResult::make(3);
    REQUIRE(ok.ok());
    REQUIRE(ok.bytes == 3);
    REQUIRE_FALSE(ok.closed());
    REQUIRE_FALSE(ok.would_block());

    auto blocked = StreamResult::fail(StreamError::WouldBlock);
    REQUIRE_FALSE(blocked.ok());
    REQUIRE(blocked.would_block());
    REQUIRE_FALSE(blocked.closed());

    auto invalid = StreamResult::fail(StreamError::Invalid);
    REQUIRE_FALSE(invalid.ok());
    REQUIRE_FALSE(invalid.closed());
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

TEST_CASE("FileStream append and move keep handle ownership correct", "[stream]") {
    auto path = make_temp_path("pulp_stream_append");
    const std::uint8_t first[] = {'a', 'b'};
    const std::uint8_t second[] = {'c', 'd', 'e'};

    {
        FileStream w(path.string(), FileStream::Mode::Write);
        REQUIRE(w.write(first, sizeof(first)).bytes == sizeof(first));
        REQUIRE(w.flush());
    }

    {
        FileStream appender(path.string(), FileStream::Mode::Append);
        REQUIRE(appender.is_open());
        FileStream moved(std::move(appender));
        REQUIRE_FALSE(appender.is_open());
        REQUIRE(moved.is_open());
        auto wrote = moved.write(second, sizeof(second));
        REQUIRE(wrote.ok());
        REQUIRE(wrote.bytes == sizeof(second));
        REQUIRE(moved.flush());
    }

    {
        FileStream r(path.string(), FileStream::Mode::Read);
        std::uint8_t out[sizeof(first) + sizeof(second)]{};
        auto got = r.read(out, sizeof(out));
        REQUIRE(got.ok());
        REQUIRE(got.bytes == sizeof(out));
        REQUIRE(std::memcmp(out, "abcde", sizeof(out)) == 0);
    }

    std::filesystem::remove(path);
}

TEST_CASE("FileStream read-write mode tracks position and zero-byte I/O",
          "[stream][coverage][phase3]") {
    auto path = make_temp_path("pulp_stream_readwrite");
    const std::uint8_t payload[] = {'r', 'w', '0'};

    FileStream stream(path.string(), FileStream::Mode::ReadWrite);
    REQUIRE(stream.is_open());
    REQUIRE(stream.position() == 0);

    REQUIRE(stream.write(payload, 0).ok());
    REQUIRE(stream.position() == 0);

    auto wrote = stream.write(payload, sizeof(payload));
    REQUIRE(wrote.ok());
    REQUIRE(wrote.bytes == sizeof(payload));
    REQUIRE(stream.position() == sizeof(payload));
    REQUIRE(stream.flush());

    stream.close();
    REQUIRE_FALSE(stream.is_open());
    REQUIRE_FALSE(stream.flush());
    REQUIRE(stream.position() == static_cast<std::size_t>(-1));

    FileStream reader(path.string(), FileStream::Mode::Read);
    REQUIRE(reader.is_open());
    std::uint8_t out[sizeof(payload)]{};
    REQUIRE(reader.read(out, 0).ok());
    REQUIRE(reader.position() == 0);

    auto got = reader.read(out, sizeof(out));
    REQUIRE(got.ok());
    REQUIRE(got.bytes == sizeof(out));
    REQUIRE(std::memcmp(out, payload, sizeof(payload)) == 0);

    reader.close();
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

TEST_CASE("FileStream zero-size operations and positions are stable", "[stream][coverage][issue-656]") {
    auto path = make_temp_path("pulp_stream_position");
    const std::uint8_t payload[] = {'x', 'y', 'z'};

    {
        FileStream stream(path.string(), FileStream::Mode::ReadWrite);
        REQUIRE(stream.is_open());
        REQUIRE(stream.position() == 0);

        auto zero_write = stream.write(payload, 0);
        REQUIRE(zero_write.ok());
        REQUIRE(zero_write.bytes == 0);
        REQUIRE(stream.position() == 0);

        auto wrote = stream.write(payload, sizeof(payload));
        REQUIRE(wrote.ok());
        REQUIRE(wrote.bytes == sizeof(payload));
        REQUIRE(stream.position() == sizeof(payload));
        REQUIRE(stream.flush());
    }

    {
        FileStream stream(path.string(), FileStream::Mode::Read);
        std::uint8_t out[3]{};
        auto zero_read = stream.read(out, 0);
        REQUIRE(zero_read.ok());
        REQUIRE(zero_read.bytes == 0);
        REQUIRE(stream.position() == 0);
    }

    FileStream closed;
    REQUIRE(closed.position() == static_cast<std::size_t>(-1));
    REQUIRE_FALSE(closed.flush());
    closed.close();
    REQUIRE_FALSE(closed.is_open());

    std::filesystem::remove(path);
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

TEST_CASE("FileStream ReadWrite mode tracks position and truncates on open",
          "[stream][file][coverage][issue-641]") {
    auto path = make_temp_path("pulp_stream_readwrite");
    const std::uint8_t first[] = {'o', 'l', 'd'};
    const std::uint8_t second[] = {'n', 'e', 'w', '!'};

    {
        FileStream seed(path.string(), FileStream::Mode::Write);
        REQUIRE(seed.write(first, sizeof(first)).bytes == sizeof(first));
        REQUIRE(seed.flush());
    }

    {
        FileStream stream(path.string(), FileStream::Mode::ReadWrite);
        REQUIRE(stream.is_open());
        REQUIRE(stream.position() == 0);
        REQUIRE(stream.write(second, sizeof(second)).bytes == sizeof(second));
        REQUIRE(stream.position() == sizeof(second));
        REQUIRE(stream.flush());
    }

    {
        FileStream readback(path.string(), FileStream::Mode::Read);
        std::array<std::uint8_t, sizeof(second)> out{};
        auto got = readback.read(out.data(), out.size());
        REQUIRE(got.ok());
        REQUIRE(got.bytes == out.size());
        REQUIRE(std::memcmp(out.data(), second, sizeof(second)) == 0);
        REQUIRE(readback.read(out.data(), out.size()).closed());
    }

    std::filesystem::remove(path);
}

TEST_CASE("MemoryStream clear keeps closed streams closed",
          "[stream][memory][coverage][issue-641]") {
    MemoryStream stream(std::vector<std::uint8_t>{1, 2, 3});
    stream.close();
    stream.clear();

    REQUIRE_FALSE(stream.is_open());
    REQUIRE(stream.size() == 0);
    REQUIRE(stream.read_position() == 0);

    std::uint8_t byte = 0;
    REQUIRE(stream.read(&byte, 1).closed());
    REQUIRE(stream.write(&byte, 1).closed());
}

TEST_CASE("MemoryStream appends without rewinding the read cursor",
          "[stream][memory][coverage][phase3]") {
    MemoryStream stream(std::vector<std::uint8_t>{1, 2, 3});
    std::uint8_t out[4]{};

    auto first = stream.read(out, 2);
    REQUIRE(first.ok());
    REQUIRE(first.bytes == 2);
    REQUIRE(stream.read_position() == 2);

    const std::uint8_t extra[] = {4, 5};
    auto wrote = stream.write(extra, sizeof(extra));
    REQUIRE(wrote.ok());
    REQUIRE(wrote.bytes == sizeof(extra));
    REQUIRE(stream.size() == 5);
    REQUIRE(stream.read_position() == 2);

    auto tail = stream.read(out, sizeof(out));
    REQUIRE(tail.ok());
    REQUIRE(tail.bytes == 3);
    REQUIRE(out[0] == 3);
    REQUIRE(out[1] == 4);
    REQUIRE(out[2] == 5);
}

TEST_CASE("MemoryStream clear allows fresh writes from the beginning",
          "[stream][memory][coverage][phase3]") {
    MemoryStream stream(std::vector<std::uint8_t>{9, 8, 7});
    std::uint8_t discarded[2]{};
    REQUIRE(stream.read(discarded, sizeof(discarded)).bytes == sizeof(discarded));
    REQUIRE(stream.read_position() == 2);

    stream.clear();
    REQUIRE(stream.is_open());
    REQUIRE(stream.size() == 0);
    REQUIRE(stream.read_position() == 0);

    const std::uint8_t fresh[] = {1, 3, 5, 7};
    REQUIRE(stream.write(fresh, sizeof(fresh)).bytes == sizeof(fresh));
    REQUIRE(stream.size() == sizeof(fresh));
    REQUIRE(stream.read_position() == 0);

    std::uint8_t out[sizeof(fresh)]{};
    REQUIRE(stream.read(out, sizeof(out)).bytes == sizeof(out));
    REQUIRE(std::memcmp(out, fresh, sizeof(fresh)) == 0);
}

TEST_CASE("MemoryStream default construction reports open empty stream",
          "[stream][memory][coverage][phase3-large]") {
    MemoryStream empty;
    REQUIRE(empty.is_open());
    REQUIRE(empty.size() == 0);
    REQUIRE(empty.read_position() == 0);

    std::uint8_t byte = 0;
    REQUIRE(empty.read(&byte, 1).closed());
}

TEST_CASE("FileStream reopen closes the previous handle",
          "[stream][file][coverage][phase3]") {
    auto first = make_temp_path("pulp_stream_reopen_first");
    auto second = make_temp_path("pulp_stream_reopen_second");
    const std::uint8_t a[] = {'a'};
    const std::uint8_t b[] = {'b', 'b'};

    FileStream stream(first.string(), FileStream::Mode::Write);
    REQUIRE(stream.is_open());
    REQUIRE(stream.write(a, sizeof(a)).ok());
    REQUIRE(stream.open(second.string(), FileStream::Mode::Write));
    REQUIRE(stream.write(b, sizeof(b)).ok());
    REQUIRE(stream.flush());
    stream.close();

    {
        FileStream first_reader(first.string(), FileStream::Mode::Read);
        std::uint8_t first_out = 0;
        REQUIRE(first_reader.read(&first_out, 1).ok());
        REQUIRE(first_out == 'a');
    }

    {
        FileStream second_reader(second.string(), FileStream::Mode::Read);
        std::uint8_t second_out[2]{};
        REQUIRE(second_reader.read(second_out, sizeof(second_out)).bytes == sizeof(second_out));
        REQUIRE(std::memcmp(second_out, b, sizeof(b)) == 0);
    }

    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

TEST_CASE("FileStream move construction transfers open handle and closes source",
          "[stream][file][coverage][phase3-large]") {
    auto path = make_temp_path("pulp_stream_move_construct");
    const std::uint8_t payload[] = {'m', 'o', 'v', 'e'};

    FileStream writer(path.string(), FileStream::Mode::Write);
    REQUIRE(writer.is_open());
    FileStream moved(std::move(writer));
    REQUIRE_FALSE(writer.is_open());
    REQUIRE(moved.is_open());
    REQUIRE(moved.write(payload, sizeof(payload)).bytes == sizeof(payload));
    REQUIRE(moved.flush());
    moved.close();

    FileStream reader(path.string(), FileStream::Mode::Read);
    std::uint8_t out[sizeof(payload)]{};
    REQUIRE(reader.read(out, sizeof(out)).bytes == sizeof(payload));
    REQUIRE(std::memcmp(out, payload, sizeof(payload)) == 0);

    std::filesystem::remove(path);
}

TEST_CASE("FileStream move assignment handles self and closed sources",
          "[stream][file][coverage][phase3]") {
    auto path = make_temp_path("pulp_stream_self_move");
    const std::uint8_t payload[] = {'s', 'e', 'l', 'f'};

    FileStream stream(path.string(), FileStream::Mode::Write);
    REQUIRE(stream.is_open());
    auto& same = stream;
    stream = std::move(same);
    REQUIRE(stream.is_open());
    REQUIRE(stream.write(payload, sizeof(payload)).bytes == sizeof(payload));
    REQUIRE(stream.flush());

    FileStream closed;
    stream = std::move(closed);
    REQUIRE_FALSE(stream.is_open());
    REQUIRE_FALSE(closed.is_open());

    {
        FileStream reader(path.string(), FileStream::Mode::Read);
        std::uint8_t out[sizeof(payload)]{};
        REQUIRE(reader.read(out, sizeof(out)).bytes == sizeof(payload));
        REQUIRE(std::memcmp(out, payload, sizeof(payload)) == 0);
    }

    std::filesystem::remove(path);
}

TEST_CASE("TcpStream closed state rejects I/O and survives move assignment",
          "[stream][tcp][coverage][issue-641]") {
    TcpStream first;
    TcpStream second;
    std::uint8_t byte = 0;

    REQUIRE_FALSE(first.is_open());
    REQUIRE(first.read(&byte, 1).closed());
    REQUIRE(first.write(&byte, 1).closed());

    second = std::move(first);
    REQUIRE_FALSE(second.is_open());
    REQUIRE(second.read(&byte, 1).closed());
    REQUIRE(second.write(&byte, 1).closed());
}

TEST_CASE("HttpStream invalid URLs fail without external transport",
          "[stream][http][coverage][issue-641]") {
    HttpStream stream;
    HttpStream::Request request;
    request.url = "not-a-url";
    request.timeout_seconds = 1;

    REQUIRE_FALSE(stream.fetch(request));
    REQUIRE_FALSE(stream.is_open());
    REQUIRE(stream.status_code() == 0);
    REQUIRE(stream.transport_error() == "Invalid URL");

    std::array<std::uint8_t, 8> out{};
    REQUIRE(stream.read(out.data(), out.size()).error == StreamError::IoError);
    REQUIRE(stream.write(out.data(), out.size()).error == StreamError::Invalid);
    REQUIRE(stream.eof());

    stream.close();
    REQUIRE(stream.read(out.data(), out.size()).closed());
}

TEST_CASE("HttpStream fetch resets a closed stream before reporting new failure",
          "[stream][http][coverage][phase3]") {
    HttpStream stream;
    stream.close();

    HttpStream::Request request;
    request.url = "ftp://127.0.0.1/nope";
    request.timeout_seconds = 1;

    REQUIRE_FALSE(stream.fetch(request));
    REQUIRE_FALSE(stream.is_open());
    REQUIRE(stream.status_code() == 0);
    REQUIRE(stream.transport_error() == "Invalid URL");

    std::uint8_t byte = 0;
    auto read = stream.read(&byte, 1);
    REQUIRE_FALSE(read.ok());
    REQUIRE(read.error == StreamError::IoError);
}

TEST_CASE("NamedPipe closed and missing endpoints fail closed", "[stream][named_pipe][issue-641]") {
    NamedPipe pipe;
    REQUIRE_FALSE(pipe.is_open());

    std::uint8_t buf[4]{};
    REQUIRE(pipe.read(buf, sizeof(buf)) == -1);
    REQUIRE(pipe.write(buf, sizeof(buf)) == -1);
    REQUIRE(pipe.write(std::string_view{"closed"}) == -1);
    REQUIRE_FALSE(pipe.read_string(8).has_value());
    pipe.close();
    REQUIRE_FALSE(pipe.is_open());

    auto missing = make_temp_path("pulp_missing_pipe");
    std::filesystem::remove(missing);
    REQUIRE_FALSE(pipe.connect_client(missing.string()));
    REQUIRE_FALSE(pipe.is_open());
    REQUIRE(pipe.read(buf, sizeof(buf)) == -1);
}

TEST_CASE("PipeStream default and moved-empty streams fail closed",
          "[stream][pipe][coverage][phase3]") {
    PipeStream stream;
    REQUIRE_FALSE(stream.is_open());

    std::uint8_t byte = 0;
    REQUIRE(stream.read(&byte, 1).closed());
    REQUIRE(stream.write(&byte, 1).closed());
    REQUIRE(stream.read(&byte, 0).closed());
    REQUIRE(stream.write(&byte, 0).closed());

    PipeStream moved(std::move(stream));
    REQUIRE_FALSE(stream.is_open());
    REQUIRE_FALSE(moved.is_open());
    REQUIRE(moved.read(&byte, 1).closed());

    PipeStream assigned;
    assigned = std::move(moved);
    REQUIRE_FALSE(assigned.is_open());
    assigned.close();
    REQUIRE_FALSE(assigned.is_open());
}

#ifndef _WIN32
TEST_CASE("NamedPipe POSIX FIFO round-trips bytes and unlinks on close",
          "[stream][named_pipe][issue-641]") {
    auto path = make_temp_path("pulp_named_pipe_roundtrip");
    std::filesystem::remove(path);

    NamedPipe server;
    REQUIRE(server.create_server(path.string()));
    REQUIRE(server.is_open());
    REQUIRE(std::filesystem::exists(path));

    NamedPipe client;
    REQUIRE(client.connect_client(path.string()));
    REQUIRE(client.is_open());

    REQUIRE(server.write(std::string_view{"ping"}) == 4);
    auto from_server = client.read_string(16);
    REQUIRE(from_server.has_value());
    REQUIRE(*from_server == "ping");

    const std::uint8_t reply[] = {'p', 'o', 'n', 'g'};
    REQUIRE(client.write(reply, sizeof(reply)) == static_cast<int>(sizeof(reply)));

    std::array<std::uint8_t, sizeof(reply)> got{};
    REQUIRE(server.read(got.data(), got.size()) == static_cast<int>(got.size()));
    REQUIRE(std::memcmp(got.data(), reply, got.size()) == 0);

    client.close();
    REQUIRE_FALSE(client.is_open());
    REQUIRE(std::filesystem::exists(path));

    server.close();
    REQUIRE_FALSE(server.is_open());
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("NamedPipe POSIX move transfers FIFO cleanup ownership",
          "[stream][named_pipe][issue-641]") {
    auto first = make_temp_path("pulp_named_pipe_move_first");
    auto second = make_temp_path("pulp_named_pipe_move_second");
    std::filesystem::remove(first);
    std::filesystem::remove(second);

    NamedPipe original;
    REQUIRE(original.create_server(first.string()));
    REQUIRE(std::filesystem::exists(first));

    NamedPipe moved(std::move(original));
    REQUIRE_FALSE(original.is_open());
    REQUIRE(moved.is_open());
    REQUIRE(std::filesystem::exists(first));

    NamedPipe replacement;
    REQUIRE(replacement.create_server(second.string()));
    REQUIRE(std::filesystem::exists(second));

    moved = std::move(replacement);
    REQUIRE_FALSE(replacement.is_open());
    REQUIRE(moved.is_open());
    REQUIRE_FALSE(std::filesystem::exists(first));
    REQUIRE(std::filesystem::exists(second));

    moved.close();
    REQUIRE_FALSE(moved.is_open());
    REQUIRE_FALSE(std::filesystem::exists(second));
}

TEST_CASE("NamedPipe POSIX create failure leaves pipe closed",
          "[stream][named_pipe][issue-641]") {
    auto directory = make_temp_path("pulp_named_pipe_directory_collision");
    std::filesystem::remove(directory);
    std::filesystem::create_directory(directory);

    NamedPipe pipe;
    REQUIRE_FALSE(pipe.create_server(directory.string()));
    REQUIRE_FALSE(pipe.is_open());

    std::uint8_t buf[1]{};
    REQUIRE(pipe.read(buf, sizeof(buf)) == -1);
    REQUIRE(pipe.write(buf, sizeof(buf)) == -1);
    pipe.close();
    std::filesystem::remove(directory);
}
#endif
