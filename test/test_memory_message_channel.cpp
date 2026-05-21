#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/memory_message_channel.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::runtime;

namespace {

class BinaryOnlyMessageChannel final : public MessageChannel {
public:
    bool send(const std::uint8_t* data, std::size_t size) override {
        payload.assign(data, data + size);
        return accepted;
    }
    void on_message(MessageCallback) override {}
    void on_closed(ChannelClosedCallback) override {}
    void on_error(ChannelErrorCallback) override {}
    void close() override { open = false; }
    bool is_open() const override { return open; }

    std::vector<std::uint8_t> payload;
    bool accepted = true;
    bool open = true;
};

} // namespace

TEST_CASE("MessageChannel default text send uses binary send",
          "[runtime][message_channel][coverage][phase3]") {
    BinaryOnlyMessageChannel channel;
    MessageChannel& base = channel;

    REQUIRE(base.send(std::string_view("plain-text")));
    REQUIRE(channel.payload == std::vector<std::uint8_t>(
                                   {'p', 'l', 'a', 'i', 'n', '-', 't', 'e', 'x', 't'}));

    channel.accepted = false;
    REQUIRE_FALSE(base.send(std::string_view("rejected")));
}

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

TEST_CASE("MemoryMessageChannel delivers empty text and binary messages",
          "[runtime][message_channel][codecov]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    std::vector<Message> received;
    right->on_message([&](const Message& message) {
        received.push_back(message);
    });

    std::uint8_t empty_payload = 0;
    REQUIRE(left->send_text(""));
    REQUIRE(left->send(&empty_payload, 0));

    REQUIRE(received.size() == 2);
    REQUIRE(received[0].kind == MessageKind::Text);
    REQUIRE(received[0].payload.empty());
    REQUIRE(received[0].as_text().empty());
    REQUIRE(received[1].kind == MessageKind::Binary);
    REQUIRE(received[1].payload.empty());
    REQUIRE(received[1].as_text().empty());
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

TEST_CASE("MemoryMessageChannel clears message callbacks",
          "[runtime][message_channel][coverage][phase3]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int deliveries = 0;
    right->on_message([&](const Message&) { ++deliveries; });
    REQUIRE(left->send_text("delivered"));
    REQUIRE(deliveries == 1);

    right->on_message({});
    REQUIRE(left->send_text("ignored"));
    REQUIRE(deliveries == 1);
}

TEST_CASE("MemoryMessageChannel send succeeds without peer callback",
          "[runtime][message_channel]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    REQUIRE(left->is_open());
    REQUIRE(right->is_open());
    REQUIRE(left->send_text("nobody-listens"));
}

TEST_CASE("MemoryMessageChannel delivers zero-length binary and text payloads",
          "[runtime][message_channel][coverage][issue-641]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    std::vector<Message> received;
    right->on_message([&](const Message& message) {
        received.push_back(message);
    });

    std::uint8_t empty = 0;
    REQUIRE(left->send(&empty, 0));
    REQUIRE(left->send_text(""));

    REQUIRE(received.size() == 2);
    REQUIRE(received[0].kind == MessageKind::Binary);
    REQUIRE(received[0].payload.empty());
    REQUIRE(received[1].kind == MessageKind::Text);
    REQUIRE(received[1].payload.empty());
    REQUIRE(received[1].as_text().empty());
}

TEST_CASE("MemoryMessageChannel rejects nonempty null binary sends",
          "[runtime][message_channel][codecov]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int deliveries = 0;
    right->on_message([&](const Message&) {
        ++deliveries;
    });

    REQUIRE_FALSE(left->send(nullptr, 1));
    REQUIRE(deliveries == 0);
    REQUIRE(left->is_open());
    REQUIRE(right->is_open());
}

TEST_CASE("MemoryMessageChannel accepts empty null binary sends",
          "[runtime][message_channel][coverage][phase3]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    std::vector<Message> received;
    right->on_message([&](const Message& message) {
        received.push_back(message);
    });

    REQUIRE(left->send(nullptr, 0));

    REQUIRE(received.size() == 1);
    REQUIRE(received.front().kind == MessageKind::Binary);
    REQUIRE(received.front().payload.empty());
}

TEST_CASE("MemoryMessageChannel can clear callbacks while remaining open",
          "[runtime][message_channel][coverage][issue-641]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int calls = 0;
    right->on_message([&](const Message&) { ++calls; });
    REQUIRE(left->send_text("before-clear"));
    REQUIRE(calls == 1);

    right->on_message({});
    REQUIRE(left->send_text("after-clear"));
    REQUIRE(calls == 1);
    REQUIRE(left->is_open());
    REQUIRE(right->is_open());
}

TEST_CASE("MemoryMessageChannel text views preserve embedded NUL bytes",
          "[runtime][message_channel][coverage][issue-641]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    std::string text = "prefix";
    text.push_back('\0');
    text += "suffix";

    std::string_view view;
    Message received;
    right->on_message([&](const Message& message) {
        received = message;
        view = received.as_text();
    });

    REQUIRE(left->send_text(text));
    REQUIRE(received.kind == MessageKind::Text);
    REQUIRE(view.size() == text.size());
    REQUIRE(std::string(view) == text);
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

TEST_CASE("MemoryMessageChannel replaces close callbacks and leaves error inert",
          "[runtime][message_channel][codecov]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int first_left_closed = 0;
    int second_left_closed = 0;
    int right_errors = 0;
    left->on_closed([&] { ++first_left_closed; });
    left->on_closed([&] { ++second_left_closed; });
    right->on_error([&](std::string_view) { ++right_errors; });

    right->close();

    REQUIRE(first_left_closed == 0);
    REQUIRE(second_left_closed == 1);
    REQUIRE(right_errors == 0);
    REQUIRE_FALSE(left->is_open());
    REQUIRE_FALSE(right->is_open());
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

TEST_CASE("MemoryMessageChannel close callback can send no further messages",
          "[runtime][message_channel][coverage][phase3]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int left_closed = 0;
    bool send_from_close = true;
    left->on_closed([&] {
        ++left_closed;
        send_from_close = left->send_text("after-close-callback");
    });

    right->close();

    REQUIRE(left_closed == 1);
    REQUIRE_FALSE(send_from_close);
    REQUIRE_FALSE(left->is_open());
    REQUIRE_FALSE(right->is_open());
}

TEST_CASE("MemoryMessageChannel sender observes peer callback replacement during delivery",
          "[runtime][message_channel][coverage][phase3]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int first_calls = 0;
    int second_calls = 0;
    right->on_message([&](const Message&) {
        ++first_calls;
        right->on_message([&](const Message&) {
            ++second_calls;
        });
    });

    REQUIRE(left->send_text("first"));
    REQUIRE(left->send_text("second"));

    REQUIRE(first_calls == 1);
    REQUIRE(second_calls == 1);
}

TEST_CASE("MemoryMessageChannel supports reentrant replies during delivery",
          "[runtime][message_channel][coverage][phase3]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    std::vector<std::string> seen;
    left->on_message([&](const Message& message) {
        seen.emplace_back("left:" + std::string(message.as_text()));
    });
    right->on_message([&](const Message& message) {
        seen.emplace_back("right:" + std::string(message.as_text()));
        REQUIRE(right->send_text("reply"));
    });

    REQUIRE(left->send_text("request"));
    REQUIRE(seen == std::vector<std::string>{"right:request", "left:reply"});
}

TEST_CASE("MemoryMessageChannel self close does not invoke a late replacement callback",
          "[runtime][message_channel][coverage][phase3]") {
    auto [left, right] = MemoryMessageChannel::make_pair();

    int closed = 0;
    left->on_closed([&] {
        ++closed;
        left->on_closed([&] { ++closed; });
    });

    left->close();
    left->close();
    right->close();

    REQUIRE(closed == 1);
}
