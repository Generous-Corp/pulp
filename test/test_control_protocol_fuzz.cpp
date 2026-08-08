#include "fuzz/control_protocol_fuzz_oracle.hpp"
#include "fuzz/control_protocol_seed_corpus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pulp::inspect;
using namespace pulp::test::control_protocol_fuzz;

namespace {

class Random {
  public:
    explicit Random(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        state_ += 0x9e3779b97f4a7c15ULL;
        auto value = state_;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    std::size_t below(std::size_t bound) {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }

  private:
    std::uint64_t state_;
};

std::uint64_t env_number(const char* name, std::uint64_t fallback) {
    const auto* raw = std::getenv(name);
    return raw && *raw ? std::strtoull(raw, nullptr, 10) : fallback;
}

std::vector<std::string> seed_corpus() {
    std::vector<std::string> corpus;
    for (auto& entry : control_protocol_seed_corpus())
        corpus.push_back(std::move(entry.bytes));
    return corpus;
}

std::string random_bytes(Random& random, std::size_t size) {
    std::string bytes(size, '\0');
    for (auto& byte : bytes)
        byte = static_cast<char>(random.next() & 0xffU);
    return bytes;
}

std::string hex_bytes(std::string_view bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 0x0f]);
    }
    return result;
}

std::string mutate(const std::vector<std::string>& corpus, Random& random, std::uint64_t index) {
    // Preserve regular accepted cases so a mutation run that only exercises
    // tokenizer rejection cannot report misleading success.
    if ((index % 16) == 0)
        return corpus[random.below(corpus.size())];
    // Exercise the decoder's pre-parse byte ceiling independently of JSON.
    if ((index % 257) == 0)
        return random_bytes(random, kControlMaximumEnvelopeBytes + 1 + random.below(256));
    if ((index & 1U) == 0)
        return random_bytes(random, random.below(2048));

    std::string bytes = corpus[random.below(corpus.size())];
    const auto operations = 1 + random.below(8);
    constexpr std::array<char, 20> interesting{'{',  '}',  '[',  ']',    ':',    ',',   '"',
                                               '\\', '0',  '9',  '-',    '.',    'e',   ' ',
                                               '\n', '\t', '\0', '\x7f', '\x80', '\xff'};
    for (std::size_t operation = 0; operation < operations; ++operation) {
        switch (random.below(6)) {
        case 0:
            if (!bytes.empty())
                bytes[random.below(bytes.size())] = interesting[random.below(interesting.size())];
            break;
        case 1:
            if (!bytes.empty()) {
                const auto begin = random.below(bytes.size());
                const auto count =
                    1 + random.below(std::min<std::size_t>(32, bytes.size() - begin));
                bytes.erase(begin, count);
            }
            break;
        case 2: {
            const auto at = random.below(bytes.size() + 1);
            bytes.insert(at, random_bytes(random, 1 + random.below(32)));
            break;
        }
        case 3:
            if (!bytes.empty())
                bytes.resize(random.below(bytes.size()));
            break;
        case 4:
            if (!bytes.empty() && bytes.size() < kControlMaximumEnvelopeBytes) {
                const auto begin = random.below(bytes.size());
                const auto count =
                    1 + random.below(std::min<std::size_t>(64, bytes.size() - begin));
                bytes.insert(begin, bytes.substr(begin, count));
            }
            break;
        case 5:
            bytes.push_back('\0');
            break;
        }
        if (bytes.size() > kControlMaximumEnvelopeBytes + 512)
            bytes.resize(kControlMaximumEnvelopeBytes + 512);
    }
    return bytes;
}

} // namespace

TEST_CASE("control protocol fuzz oracle accepts every closed seed envelope",
          "[inspect][control-protocol][fuzz]") {
    const auto named_corpus = control_protocol_seed_corpus();
    REQUIRE(named_corpus.size() == 6);
    for (const auto& entry : named_corpus) {
        INFO(entry.filename);
        REQUIRE_FALSE(entry.bytes.empty());
        const auto finding = inspect(entry.bytes);
        INFO(format_finding(finding));
        REQUIRE_FALSE(finding);
        const auto envelope = decode_control_envelope(entry.bytes);
        REQUIRE(envelope);
        if (entry.filename == "request.json") {
            const auto* request = std::get_if<ControlRequestEnvelope>(&envelope->payload);
            REQUIRE(request);
            CHECK(control_request_hash(*request) == request->request_hash);
        }
    }
}

TEST_CASE("control protocol decoder survives bounded arbitrary byte properties",
          "[inspect][control-protocol][fuzz]") {
    const auto corpus = seed_corpus();
    REQUIRE_FALSE(corpus.empty());
    const auto seed = env_number("PULP_CONTROL_PROTOCOL_FUZZ_SEED", 0xc071'4010'0000'0001ULL);
    const auto cases = env_number("PULP_CONTROL_PROTOCOL_FUZZ_CASES", 5'000);
    Random random(seed);
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t oversized = 0;
    for (std::uint64_t index = 0; index < cases; ++index) {
        const auto input = mutate(corpus, random, index);
        INFO("reproduce with PULP_CONTROL_PROTOCOL_FUZZ_SEED="
             << seed << " case=" << index << " bytes_hex=" << hex_bytes(input));
        const auto finding = inspect(input);
        INFO("reproduce with PULP_CONTROL_PROTOCOL_FUZZ_SEED=" << seed << " case=" << index
                                                               << " bytes=" << input.size() << ": "
                                                               << format_finding(finding));
        REQUIRE_FALSE(finding);
        if (input.size() > kControlMaximumEnvelopeBytes) {
            ++oversized;
        } else if (decode_control_envelope(input)) {
            ++accepted;
        } else {
            ++rejected;
        }
    }
    INFO("accepted=" << accepted << " rejected=" << rejected << " oversized=" << oversized);
    REQUIRE(accepted > 0);
    REQUIRE(rejected > 0);
    if (cases >= 257)
        REQUIRE(oversized > 0);
}

TEST_CASE("control protocol fuzz byte ceiling rejects before JSON parsing",
          "[inspect][control-protocol][fuzz][bounds]") {
    std::string oversized(kControlMaximumEnvelopeBytes + 1, '{');
    ControlProtocolDiagnostics diagnostics;
    CHECK_FALSE(decode_control_envelope(oversized, &diagnostics));
    CHECK(diagnostics.code == ControlProtocolError::EnvelopeTooLarge);
    CHECK_FALSE(inspect(oversized));
}
