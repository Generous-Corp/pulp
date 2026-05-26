// MIDI-CI Property Exchange (PE) — framing, Mcoded7, chunker, reassembly,
// JSON header, subscription manager. Targets the spec-compliance gap from
// macOS plugin authoring plan item 8.4.

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/mcoded7.hpp>
#include <pulp/midi/midi_ci.hpp>
#include <pulp/midi/midi_ci_pe.hpp>

#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace pulp::midi;

// ── Mcoded7 ─────────────────────────────────────────────────────────────

TEST_CASE("Mcoded7 round-trips empty input",
          "[midi][ci][pe][mcoded7][issue-84]") {
    auto enc = mcoded7_encode(nullptr, 0);
    REQUIRE(enc.empty());
    auto dec = mcoded7_decode(nullptr, 0);
    REQUIRE(dec.empty());
}

TEST_CASE("Mcoded7 round-trips full groups",
          "[midi][ci][pe][mcoded7][issue-84]") {
    std::vector<uint8_t> input;
    for (int i = 0; i < 14; ++i) input.push_back(static_cast<uint8_t>(0x80 + i));
    auto enc = mcoded7_encode(input);
    // 14 bytes -> 2 groups of (1 hi + 7 lo) = 16 bytes.
    REQUIRE(enc.size() == 16);
    for (auto b : enc) REQUIRE((b & 0x80) == 0);  // all <= 0x7F
    auto dec = mcoded7_decode(enc);
    REQUIRE(dec == input);
}

TEST_CASE("Mcoded7 round-trips short final group",
          "[midi][ci][pe][mcoded7][issue-84]") {
    std::vector<uint8_t> input{0xFF, 0x80, 0x01, 0x02};  // 4 bytes
    auto enc = mcoded7_encode(input);
    REQUIRE(enc.size() == 5);  // 1 hi + 4 lo
    // Top two bits of hi byte mark the two 0x80+ inputs.
    REQUIRE(enc[0] == 0b00001100);
    auto dec = mcoded7_decode(enc);
    REQUIRE(dec == input);
}

TEST_CASE("Mcoded7 round-trips arbitrary binary payload",
          "[midi][ci][pe][mcoded7][issue-84]") {
    std::vector<uint8_t> payload(1000);
    std::mt19937 rng(42);
    for (auto& b : payload) b = static_cast<uint8_t>(rng() & 0xFF);
    auto enc = mcoded7_encode(payload);
    for (auto b : enc) REQUIRE((b & 0x80) == 0);
    auto dec = mcoded7_decode(enc);
    REQUIRE(dec == payload);
}

TEST_CASE("Mcoded7 rejects high-bit byte in stream",
          "[midi][ci][pe][mcoded7][issue-84]") {
    std::vector<uint8_t> bad{0x00, 0x80};  // body byte has bit-7 set
    REQUIRE(mcoded7_decode(bad).empty());

    std::vector<uint8_t> bad_hi{0x81, 0x00};  // hi byte itself has bit-7 set
    REQUIRE(mcoded7_decode(bad_hi).empty());
}

TEST_CASE("Mcoded7 rejects lone trailing high-bit byte",
          "[midi][ci][pe][mcoded7][issue-84]") {
    std::vector<uint8_t> bad{0x10};  // hi byte with no body
    REQUIRE(mcoded7_decode(bad).empty());
}

// ── PE message build/parse ──────────────────────────────────────────────

TEST_CASE("PE build/parse round-trips a single chunk",
          "[midi][ci][pe][issue-84]") {
    MUID src{0x01234567};
    MUID dst{0x07654321};
    std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF};
    auto header = pe_header_make("/DeviceInfo", "full", 200);

    auto msg = pe_build_message(PeMessageType::GetReply, 2, src, dst,
                                /*request_id*/ 0x12, header,
                                /*total*/ 1, /*num*/ 1, payload);

    REQUIRE(msg.front() == 0xF0);
    REQUIRE(msg.back() == 0xF7);
    REQUIRE(msg[4] == static_cast<uint8_t>(PeMessageType::GetReply));
    for (std::size_t i = 1; i + 1 < msg.size(); ++i) {
        REQUIRE((msg[i] & 0x80) == 0);  // body must be 7-bit clean
    }

    auto parsed = pe_parse_message(msg.data(), msg.size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->request_id == 0x12);
    REQUIRE(parsed->total_chunks == 1);
    REQUIRE(parsed->chunk_number == 1);
    REQUIRE(parsed->header_json == header);
    REQUIRE(parsed->payload == payload);
}

TEST_CASE("PE parse rejects malformed envelopes",
          "[midi][ci][pe][issue-84]") {
    std::vector<uint8_t> too_short{0xF0, 0x7E};
    REQUIRE_FALSE(pe_parse_message(too_short.data(), too_short.size()).has_value());

    REQUIRE_FALSE(pe_parse_message(nullptr, 0).has_value());

    // Build then corrupt sub-ID.
    auto good = pe_build_message(PeMessageType::GetInquiry, 2,
                                 MUID{1}, MUID{2}, 1, "{\"resource\":\"x\"}",
                                 1, 1, nullptr, 0);
    auto wrong_subid = good;
    wrong_subid[4] = 0x70;  // discovery, not PE
    REQUIRE_FALSE(pe_parse_message(wrong_subid.data(),
                                   wrong_subid.size()).has_value());

    auto truncated = good;
    truncated.pop_back();  // drop F7
    REQUIRE_FALSE(pe_parse_message(truncated.data(),
                                   truncated.size()).has_value());
}

// ── Chunker ─────────────────────────────────────────────────────────────

TEST_CASE("PE chunker splits payload into bounded chunks",
          "[midi][ci][pe][issue-84]") {
    std::vector<uint8_t> payload(250);
    std::iota(payload.begin(), payload.end(), uint8_t{0});

    auto chunks = pe_split_into_chunks(PeMessageType::GetReply, 2,
                                       MUID{1}, MUID{2}, /*req*/ 7,
                                       pe_header_make("/Big", "partial"),
                                       payload.data(), payload.size(),
                                       /*max_payload_bytes*/ 64);
    REQUIRE(chunks.size() == 4);  // ceil(250/64) = 4

    PeReassembler ra;
    std::optional<PeChunk> done;
    for (auto& wire : chunks) {
        auto p = pe_parse_message(wire.data(), wire.size());
        REQUIRE(p.has_value());
        REQUIRE(p->request_id == 7);
        REQUIRE(p->total_chunks == 4);
        done = ra.push(*p);
    }
    REQUIRE(done.has_value());
    REQUIRE(done->payload == payload);
}

TEST_CASE("PE chunker emits one chunk for empty payload",
          "[midi][ci][pe][issue-84]") {
    auto chunks = pe_split_into_chunks(PeMessageType::SubscribeInquiry, 2,
                                       MUID{1}, MUID{2}, 5,
                                       pe_header_make("/Stub", "start"),
                                       nullptr, 0, 32);
    REQUIRE(chunks.size() == 1);
    auto p = pe_parse_message(chunks[0].data(), chunks[0].size());
    REQUIRE(p.has_value());
    REQUIRE(p->total_chunks == 1);
    REQUIRE(p->chunk_number == 1);
    REQUIRE(p->payload.empty());
}

TEST_CASE("PeReassembler handles out-of-order chunks",
          "[midi][ci][pe][issue-84]") {
    std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto chunks = pe_split_into_chunks(PeMessageType::GetReply, 2,
                                       MUID{1}, MUID{2}, 9,
                                       pe_header_make("/X"),
                                       payload.data(), payload.size(),
                                       /*max*/ 3);
    REQUIRE(chunks.size() == 4);

    PeReassembler ra;
    // Feed in reverse.
    std::optional<PeChunk> done;
    for (std::size_t i = chunks.size(); i-- > 0;) {
        auto p = pe_parse_message(chunks[i].data(), chunks[i].size());
        REQUIRE(p.has_value());
        done = ra.push(*p);
    }
    REQUIRE(done.has_value());
    REQUIRE(done->payload == payload);
}

TEST_CASE("PeReassembler ignores duplicate chunks",
          "[midi][ci][pe][issue-84]") {
    std::vector<uint8_t> payload{42, 43, 44};
    auto chunks = pe_split_into_chunks(PeMessageType::GetReply, 2,
                                       MUID{1}, MUID{2}, 4,
                                       pe_header_make("/Y"),
                                       payload.data(), payload.size(), 1);
    REQUIRE(chunks.size() == 3);

    PeReassembler ra;
    auto p0 = pe_parse_message(chunks[0].data(), chunks[0].size());
    auto p1 = pe_parse_message(chunks[1].data(), chunks[1].size());
    auto p2 = pe_parse_message(chunks[2].data(), chunks[2].size());

    REQUIRE_FALSE(ra.push(*p0).has_value());
    REQUIRE_FALSE(ra.push(*p0).has_value());  // dup
    REQUIRE_FALSE(ra.push(*p1).has_value());
    auto done = ra.push(*p2);
    REQUIRE(done.has_value());
    REQUIRE(done->payload == payload);
    REQUIRE(ra.pending_transfers() == 0);
}

TEST_CASE("PeReassembler cancels in-flight transfers",
          "[midi][ci][pe][issue-84]") {
    PeReassembler ra;
    auto chunks = pe_split_into_chunks(PeMessageType::GetReply, 2,
                                       MUID{1}, MUID{2}, 11,
                                       pe_header_make("/Z"),
                                       reinterpret_cast<const uint8_t*>("hi"),
                                       2, 1);
    auto p0 = pe_parse_message(chunks[0].data(), chunks[0].size());
    ra.push(*p0);
    REQUIRE(ra.pending_transfers() == 1);
    ra.cancel(11);
    REQUIRE(ra.pending_transfers() == 0);
}

TEST_CASE("PeReassembler rejects mismatched total_chunks mid-transfer",
          "[midi][ci][pe][issue-84]") {
    PeReassembler ra;
    PeChunk a{};
    a.request_id = 1;
    a.total_chunks = 3;
    a.chunk_number = 1;
    a.payload = {1};
    PeChunk b = a;
    b.total_chunks = 5;
    b.chunk_number = 2;
    b.payload = {2};

    REQUIRE_FALSE(ra.push(a).has_value());
    REQUIRE_FALSE(ra.push(b).has_value());  // abort
    REQUIRE(ra.pending_transfers() == 0);
}

// ── JSON header ─────────────────────────────────────────────────────────

TEST_CASE("PE header build is parseable",
          "[midi][ci][pe][header][issue-84]") {
    auto h = pe_header_make("/DeviceInfo", "full", 200);
    std::string r, c;
    int s = 0;
    REQUIRE(pe_header_parse(h, &r, &c, &s));
    REQUIRE(r == "/DeviceInfo");
    REQUIRE(c == "full");
    REQUIRE(s == 200);
}

TEST_CASE("PE header omits empty command",
          "[midi][ci][pe][header][issue-84]") {
    auto h = pe_header_make("/X");
    REQUIRE(h.find("command") == std::string::npos);
    std::string r, c;
    int s = 0;
    REQUIRE(pe_header_parse(h, &r, &c, &s));
    REQUIRE(r == "/X");
    REQUIRE(c.empty());
    REQUIRE(s == 200);
}

TEST_CASE("PE header escapes special chars in resource",
          "[midi][ci][pe][header][issue-84]") {
    auto h = pe_header_make("/with\"quote\\and\nnewline", "set", 404);
    // Build->parse must round-trip the escaped string verbatim.
    std::string r, c;
    int s = 0;
    REQUIRE(pe_header_parse(h, &r, &c, &s));
    REQUIRE(r == "/with\"quote\\and\nnewline");
    REQUIRE(c == "set");
    REQUIRE(s == 404);
}

TEST_CASE("PE header parser handles negative status",
          "[midi][ci][pe][header][issue-84]") {
    std::string json = "{\"resource\":\"/x\",\"status\":-1}";
    std::string r, c;
    int s = 0;
    REQUIRE(pe_header_parse(json, &r, &c, &s));
    REQUIRE(r == "/x");
    REQUIRE(s == -1);
}

TEST_CASE("PE header parser returns false on empty input",
          "[midi][ci][pe][header][issue-84]") {
    std::string r, c;
    int s = 0;
    REQUIRE_FALSE(pe_header_parse("", &r, &c, &s));
}

// ── Subscription manager ────────────────────────────────────────────────

TEST_CASE("PeSubscriptionManager tracks resource subscribers",
          "[midi][ci][pe][subscribe][issue-84]") {
    PeSubscriptionManager mgr;
    auto id1 = mgr.subscribe("/Mix/Gain", MUID{0x11});
    auto id2 = mgr.subscribe("/Mix/Gain", MUID{0x22});
    auto id3 = mgr.subscribe("/Mix/Pan",  MUID{0x33});

    REQUIRE(id1 != id2);
    REQUIRE(id2 != id3);

    auto gain_subs = mgr.subscribers_of("/Mix/Gain");
    REQUIRE(gain_subs.size() == 2);

    auto pan_subs = mgr.subscribers_of("/Mix/Pan");
    REQUIRE(pan_subs.size() == 1);
    REQUIRE(pan_subs[0].subscriber == MUID{0x33});

    REQUIRE(mgr.unsubscribe(id2));
    REQUIRE(mgr.subscribers_of("/Mix/Gain").size() == 1);
    REQUIRE_FALSE(mgr.unsubscribe("nonexistent-id"));
}

TEST_CASE("PeSubscriptionManager fans out notify payload",
          "[midi][ci][pe][subscribe][issue-84]") {
    PeSubscriptionManager mgr;
    auto id_a = mgr.subscribe("/Param/1", MUID{0xA});
    (void)id_a;
    mgr.subscribe("/Param/1", MUID{0xB});

    // Caller builds Notify messages addressed to each subscriber.
    auto subs = mgr.subscribers_of("/Param/1");
    REQUIRE(subs.size() == 2);

    std::vector<std::vector<uint8_t>> outbox;
    std::vector<uint8_t> payload{0x77};
    for (const auto& s : subs) {
        outbox.push_back(pe_build_message(
            PeMessageType::Notify, 2, MUID{0xFFFE}, s.subscriber,
            1, pe_header_make(s.resource, "notify"),
            1, 1, payload));
    }
    REQUIRE(outbox.size() == 2);
    auto parsed = pe_parse_message(outbox[0].data(), outbox[0].size());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->payload == payload);
}

// ── End-to-end: virtual responder ──────────────────────────────────────

TEST_CASE("PE chunked Get round-trips against an in-process virtual responder",
          "[midi][ci][pe][e2e][issue-84]") {
    // Simulate a Get/Reply where the responder's resource is bigger than
    // a single chunk (this is what the spec calls out as the gap).
    std::vector<uint8_t> resource_bytes(500);
    std::mt19937 rng(7);
    for (auto& b : resource_bytes) b = static_cast<uint8_t>(rng() & 0xFF);

    const MUID client{0xC1};
    const MUID server{0x5E};
    const uint8_t req_id = 0x21;

    // 1. Client sends GetInquiry (empty payload, header carries resource path).
    auto get_msgs = pe_split_into_chunks(
        PeMessageType::GetInquiry, 2, client, server, req_id,
        pe_header_make("/Patch/Current", "full"),
        nullptr, 0, 64);
    REQUIRE(get_msgs.size() == 1);

    auto get_parsed = pe_parse_message(get_msgs[0].data(), get_msgs[0].size());
    REQUIRE(get_parsed.has_value());

    // 2. Server fans out a multi-chunk GetReply with the resource bytes.
    auto reply_msgs = pe_split_into_chunks(
        PeMessageType::GetReply, 2, server, client, req_id,
        pe_header_make("/Patch/Current", "full", 200),
        resource_bytes.data(), resource_bytes.size(), 128);
    REQUIRE(reply_msgs.size() == 4);  // ceil(500/128)

    // 3. Client reassembles.
    PeReassembler ra;
    std::optional<PeChunk> done;
    for (auto& m : reply_msgs) {
        auto p = pe_parse_message(m.data(), m.size());
        REQUIRE(p.has_value());
        done = ra.push(*p);
    }
    REQUIRE(done.has_value());
    REQUIRE(done->request_id == req_id);
    REQUIRE(done->payload == resource_bytes);

    std::string r, c;
    int status = 0;
    REQUIRE(pe_header_parse(done->header_json, &r, &c, &status));
    REQUIRE(r == "/Patch/Current");
    REQUIRE(status == 200);
}

// ── pe_compress / pe_decompress (macOS plan §8.4, zlib payload) ─────────

TEST_CASE("pe_compress round-trips random payloads",
          "[midi][ci][pe][zlib][issue-84]") {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (std::size_t n : {0u, 1u, 16u, 64u, 1024u, 4096u}) {
        std::vector<uint8_t> payload(n);
        for (std::size_t i = 0; i < n; ++i)
            payload[i] = static_cast<uint8_t>(byte_dist(rng));

        auto compressed = pe_compress(payload);
        REQUIRE(compressed.has_value());
        // RFC 1950 zlib header CMF byte: low nibble must be 8 (deflate).
        if (n > 0) {
            REQUIRE_FALSE(compressed->empty());
            REQUIRE((compressed->at(0) & 0x0F) == 0x08);
        }
        auto decompressed = pe_decompress(*compressed);
        REQUIRE(decompressed.has_value());
        REQUIRE(*decompressed == payload);
    }
}

TEST_CASE("pe_compress shrinks highly compressible payload",
          "[midi][ci][pe][zlib][issue-84]") {
    // Highly repetitive input — zlib should achieve > 10x compression.
    std::vector<uint8_t> payload(4096, 0x42);
    auto compressed = pe_compress(payload);
    REQUIRE(compressed.has_value());
    REQUIRE(compressed->size() < payload.size() / 4);
    auto decompressed = pe_decompress(*compressed);
    REQUIRE(decompressed.has_value());
    REQUIRE(*decompressed == payload);
}

TEST_CASE("pe_decompress rejects garbage input",
          "[midi][ci][pe][zlib][issue-84]") {
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    auto out = pe_decompress(garbage);
    REQUIRE_FALSE(out.has_value());
}

// ── CiDiscovery Subscribe/Notify dispatcher (macOS plan §8.4) ────────────

TEST_CASE("CiDiscovery wires Subscribe to PeSubscriptionManager",
          "[midi][ci][pe][subscribe][issue-84]") {
    // Server side.
    CiDiscovery server;
    CiDeviceInfo srv_info;
    srv_info.muid = {0x11111};
    server.set_device_info(srv_info);

    // Client builds a Subscribe Inquiry for resource "/Patch".
    MUID client_muid{0x22222};
    auto subscribe_msg = pe_build_message(
        PeMessageType::SubscribeInquiry, /*ci_version=*/2,
        client_muid, srv_info.muid,
        /*request_id=*/7,
        pe_header_make("/Patch", "start", 200),
        /*total_chunks=*/1, /*chunk_number=*/1,
        nullptr, 0);

    auto reply = server.process_message(subscribe_msg.data(), subscribe_msg.size());
    REQUIRE_FALSE(reply.empty());

    // The Subscribe registered.
    REQUIRE(server.subscription_manager().all().size() == 1);
    REQUIRE(server.subscription_manager().all()[0].resource == "/Patch");
    REQUIRE(server.subscription_manager().all()[0].subscriber == client_muid);

    // The reply parses as a SubscribeReply and contains the
    // server-assigned subscribeId in its header.
    auto parsed_reply = pe_parse_message(reply.data(), reply.size());
    REQUIRE(parsed_reply.has_value());
    REQUIRE(parsed_reply->header_json.find("subscribeId") != std::string::npos);
    REQUIRE(parsed_reply->header_json.find("/Patch") != std::string::npos);
}

TEST_CASE("CiDiscovery.notify fans out to subscribers via callback",
          "[midi][ci][pe][notify][issue-84]") {
    CiDiscovery server;
    CiDeviceInfo srv_info;
    srv_info.muid = {0xABCDE};
    server.set_device_info(srv_info);

    // Register two subscribers programmatically.
    MUID sub_a{0x10001};
    MUID sub_b{0x10002};
    server.subscription_manager().subscribe("/State", sub_a);
    server.subscription_manager().subscribe("/State", sub_b);
    // A third subscriber on a *different* resource — must not fire.
    server.subscription_manager().subscribe("/Other", {0x10003});

    std::vector<MUID> notified;
    std::vector<std::string> resources;
    server.on_pe_notify = [&](MUID m, std::string_view r, std::string_view,
                              const std::vector<uint8_t>&) {
        notified.push_back(m);
        resources.emplace_back(r);
    };

    std::vector<uint8_t> payload = {1, 2, 3, 4};
    std::size_t n = server.notify("/State",
                                  pe_header_make("/State", "notify"),
                                  payload);
    REQUIRE(n == 2);
    REQUIRE(notified.size() == 2);
    REQUIRE(((notified[0] == sub_a && notified[1] == sub_b)
          || (notified[0] == sub_b && notified[1] == sub_a)));
    REQUIRE(resources[0] == "/State");
}

TEST_CASE("CiDiscovery routes incoming PropertyNotify to on_pe_notify",
          "[midi][ci][pe][notify][issue-84]") {
    CiDiscovery client;
    CiDeviceInfo info;
    info.muid = {0xDEADBE};
    client.set_device_info(info);

    bool fired = false;
    MUID seen_source{0};
    std::string seen_resource;
    std::vector<uint8_t> seen_payload;
    client.on_pe_notify = [&](MUID m, std::string_view r, std::string_view,
                              const std::vector<uint8_t>& payload) {
        fired = true;
        seen_source = m;
        seen_resource = std::string(r);
        seen_payload = payload;
    };

    MUID publisher{0x55555};
    std::vector<uint8_t> payload = {9, 8, 7};
    auto notify_msg = pe_build_message(
        PeMessageType::Notify, /*ci_version=*/2,
        publisher, info.muid,
        /*request_id=*/3,
        pe_header_make("/Topic", "notify"),
        /*total_chunks=*/1, /*chunk_number=*/1,
        payload.data(), payload.size());

    auto reply = client.process_message(notify_msg.data(), notify_msg.size());
    REQUIRE(reply.empty());  // Notify is one-way.
    REQUIRE(fired);
    REQUIRE(seen_source == publisher);
    REQUIRE(seen_resource == "/Topic");
    REQUIRE(seen_payload == payload);
}

TEST_CASE("CiDiscovery.notify with no subscribers is a no-op",
          "[midi][ci][pe][notify][issue-84]") {
    CiDiscovery server;
    int fires = 0;
    server.on_pe_notify = [&](MUID, std::string_view, std::string_view,
                              const std::vector<uint8_t>&) { ++fires; };
    std::size_t n = server.notify("/nothing", "{}", {});
    REQUIRE(n == 0);
    REQUIRE(fires == 0);
}
