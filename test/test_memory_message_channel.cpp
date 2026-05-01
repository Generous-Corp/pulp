#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/memory_message_channel.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::runtime;

TEST_CASE("MemoryMessageChannel delivers binary payloads synchronously",
          "[runtime][message_channel]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    Message received;
    int call_count = 0;
    right->on_message([&](const Message& message) {
        received = message;
        ++call_count;
    });

    std::uint8_t bytes[] = {0x01, 0x02, 0x7f};
    REQUIRE(left->send(bytes, sizeof(bytes)));

    REQUIRE(call_count == 1);
    REQUIRE(received.kind == MessageKind::Binary);
    REQUIRE(received.payload == std::vector<std::uint8_t>({0x01, 0x02, 0x7f}));
}

TEST_CASE("MemoryMessageChannel delivers text payloads and as_text views",
          "[runtime][message_channel]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    std::string text;
    MessageKind kind = MessageKind::Binary;
    right->on_message([&](const Message& message) {
        kind = message.kind;
        text.assign(message.as_text());
    });

    MessageChannel& channel = *left;
    REQUIRE(channel.send(std::string_view("hello")));

    REQUIRE(kind == MessageKind::Text);
    REQUIRE(text == "hello");
}

TEST_CASE("MemoryMessageChannel replaces message callbacks",
          "[runtime][message_channel]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int first_count = 0;
    int second_count = 0;
    right->on_message([&](const Message&) { ++first_count; });
    right->on_message([&](const Message&) { ++second_count; });

    REQUIRE(left->send_text("replace"));
    REQUIRE(first_count == 0);
    REQUIRE(second_count == 1);
}

TEST_CASE("MemoryMessageChannel send succeeds without peer callback",
          "[runtime][message_channel]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    REQUIRE(left->is_open());
    REQUIRE(right->is_open());
    REQUIRE(left->send_text("nobody-listens"));
}

TEST_CASE("MemoryMessageChannel close is peer-wide and idempotent",
          "[runtime][message_channel]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int left_closed = 0;
    int right_closed = 0;
    left->on_closed([&] { ++left_closed; });
    right->on_closed([&] { ++right_closed; });

    left->close();
    left->close();
    right->close();

    REQUIRE(left_closed == 1);
    REQUIRE(right_closed == 1);
    REQUIRE_FALSE(left->is_open());
    REQUIRE_FALSE(right->is_open());
    REQUIRE_FALSE(left->send_text("after-close"));

    std::uint8_t byte = 0;
    REQUIRE_FALSE(right->send(&byte, 1));
}

TEST_CASE("MemoryMessageChannel peer destruction closes the survivor",
          "[runtime][message_channel]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int left_closed = 0;
    int right_closed = 0;
    left->on_closed([&] { ++left_closed; });
    right->on_closed([&] { ++right_closed; });

    right.reset();

    REQUIRE(left_closed == 1);
    REQUIRE(right_closed == 1);
    REQUIRE_FALSE(left->is_open());
    REQUIRE_FALSE(left->send_text("after-peer-destroy"));

    left->close();
    REQUIRE(left_closed == 1);
}
