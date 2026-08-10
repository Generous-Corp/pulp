#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_consent_authority.hpp>

#include <chrono>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

ControlGrantConsentRequest request() {
    return {
        .client_peer = {.role = ControlPeerRole::Client,
                        .user_id = "501",
                        .process_id = 42,
                        .process_start_id = "process-generation-a",
                        .executable_identity = "dev.pulp.cli",
                        .publisher_id = "pulp-test"},
        .client_peer_fingerprint = "peer-fingerprint-a",
        .grant = {.client_id = {"client-a"},
                  .registration_id = {"registration-a"},
                  .capabilities = {InspectorCapability::TraceSessionControl},
                  .ttl = 15min},
        .registration = {.registration_id = {"registration-a"},
                         .broker_id = {"broker-a"},
                         .session_id = "session-a",
                         .instance_id = "instance-a",
                         .publication_id = "publication-a",
                         .plugin_id = "dev.pulp.plugin",
                         .publisher_id = "pulp-test",
                         .manifest_digest = std::string(64, 'a'),
                         .artifact_digest = std::string(64, 'b'),
                         .consent_identity = "consent-a",
                         .build_id = "build-a"},
        .selector_kind = ControlGrantSelectorKind::Operation,
        .selector_id = "dev.pulp.trace/session-control@1",
    };
}

ControlBrokerConsentAuthorityConfig config_for(ControlConsentPromptResult result,
                                               std::chrono::steady_clock::time_point& now,
                                               std::uint8_t& entropy_seed) {
    return {
        .prompt_timeout = 2s,
        .global_prompt_cooldown = 0ms,
        .client_prompt_cooldown = 0ms,
        .clock = [&] { return now; },
        .entropy = [&](std::size_t size) -> std::optional<std::vector<std::uint8_t>> {
            return std::vector<std::uint8_t>(size, entropy_seed++);
        },
        .prompt =
            [result](const ControlConsentPrompt& prompt, std::chrono::milliseconds timeout) {
                const auto& observed = prompt.request;
                CHECK(observed.registration.instance_id == "instance-a");
                CHECK(observed.selector_id == "dev.pulp.trace/session-control@1");
                CHECK(timeout == 2s);
                return result;
            },
    };
}

} // namespace

TEST_CASE("broker consent authority approves only an exact bounded user decision",
          "[inspect][control][consent]") {
    auto now = std::chrono::steady_clock::time_point{10s};
    std::uint8_t entropy = 1;
    ControlBrokerConsentAuthority authority{
        config_for(ControlConsentPromptResult::Approved, now, entropy)};

    const auto approved = authority.decide(request());
    REQUIRE(approved.approved);
    CHECK(approved.authority == ControlConsentAuthority::BrokerUserPrompt);
    CHECK(approved.decision_id.starts_with("broker-prompt-"));
    CHECK(approved.expires_at == now + 2s);

    const auto second = authority.decide(request());
    REQUIRE(second.approved);
    CHECK(second.decision_id != approved.decision_id);
}

TEST_CASE("broker consent authority fails closed on deny timeout and expired approval",
          "[inspect][control][consent]") {
    for (const auto result :
         {ControlConsentPromptResult::Denied, ControlConsentPromptResult::TimedOut,
          ControlConsentPromptResult::Unavailable}) {
        auto now = std::chrono::steady_clock::time_point{10s};
        std::uint8_t entropy = 1;
        ControlBrokerConsentAuthority authority{config_for(result, now, entropy)};
        CHECK_FALSE(authority.decide(request()).approved);
    }

    auto now = std::chrono::steady_clock::time_point{10s};
    std::uint8_t entropy = 1;
    auto config = config_for(ControlConsentPromptResult::Approved, now, entropy);
    config.prompt = [&](const ControlConsentPrompt&, std::chrono::milliseconds) {
        now += 2s;
        return ControlConsentPromptResult::Approved;
    };
    ControlBrokerConsentAuthority authority{std::move(config)};
    CHECK_FALSE(authority.decide(request()).approved);
}

TEST_CASE("broker consent authority invalidates decisions across reset and restart",
          "[inspect][control][consent][restart]") {
    auto now = std::chrono::steady_clock::time_point{10s};
    std::uint8_t entropy = 1;
    ControlBrokerConsentAuthority* active = nullptr;
    auto config = config_for(ControlConsentPromptResult::Approved, now, entropy);
    config.prompt = [&](const ControlConsentPrompt&, std::chrono::milliseconds) {
        active->reset();
        return ControlConsentPromptResult::Approved;
    };
    ControlBrokerConsentAuthority invalidated{std::move(config)};
    active = &invalidated;
    CHECK_FALSE(invalidated.decide(request()).approved);

    auto first_config = config_for(ControlConsentPromptResult::Approved, now, entropy);
    ControlBrokerConsentAuthority before_restart{std::move(first_config)};
    const auto before = before_restart.decide(request());
    REQUIRE(before.approved);
    auto second_config = config_for(ControlConsentPromptResult::Approved, now, entropy);
    ControlBrokerConsentAuthority after_restart{std::move(second_config)};
    const auto after = after_restart.decide(request());
    REQUIRE(after.approved);
    CHECK(after.decision_id != before.decision_id);
}

TEST_CASE("broker consent authority revalidates expiry and reset after decision entropy",
          "[inspect][control][consent][restart]") {
    auto now = std::chrono::steady_clock::time_point{10s};
    unsigned entropy_calls = 0;
    std::uint8_t unused_seed = 1;
    auto expiring = config_for(ControlConsentPromptResult::Approved, now, unused_seed);
    expiring.entropy = [&](std::size_t size) -> std::optional<std::vector<std::uint8_t>> {
        if (++entropy_calls == 2)
            now += 2s;
        return std::vector<std::uint8_t>(size, 1);
    };
    ControlBrokerConsentAuthority expired{std::move(expiring)};
    CHECK_FALSE(expired.decide(request()).approved);

    now = std::chrono::steady_clock::time_point{10s};
    entropy_calls = 0;
    ControlBrokerConsentAuthority* active = nullptr;
    auto resetting = config_for(ControlConsentPromptResult::Approved, now, unused_seed);
    resetting.entropy = [&](std::size_t size) -> std::optional<std::vector<std::uint8_t>> {
        if (++entropy_calls == 2)
            active->reset();
        return std::vector<std::uint8_t>(size, 1);
    };
    ControlBrokerConsentAuthority reset{std::move(resetting)};
    active = &reset;
    CHECK_FALSE(reset.decide(request()).approved);
}

TEST_CASE("broker consent prompt escapes deceptive host and client display text",
          "[inspect][control][consent][security]") {
    auto now = std::chrono::steady_clock::time_point{10s};
    std::uint8_t entropy = 1;
    auto hostile = request();
    hostile.client_peer.executable_identity = "trusted\nAllow everything";
    hostile.client_peer.publisher_id = "good\xe2\x80\xae"
                                       "evil";
    hostile.registration.plugin_id = "plugin\rDENY";
    hostile.registration.instance_id = "instance\tspoof";
    hostile.selector_id = "develop\nAllow";
    auto config = config_for(ControlConsentPromptResult::Approved, now, entropy);
    config.prompt = [](const ControlConsentPrompt& prompt, std::chrono::milliseconds) {
        CHECK(prompt.request.registration.plugin_id == "plugin\rDENY");
        CHECK(prompt.client_executable_display == "trusted\\x0aAllow\\x20everything");
        CHECK(prompt.client_publisher_display == "good\\xe2\\x80\\xaeevil");
        CHECK(prompt.plugin_display == "plugin\\x0dDENY");
        CHECK(prompt.instance_display == "instance\\x09spoof");
        CHECK(prompt.target_publisher_display == "pulp-test");
        CHECK(prompt.build_display == "build-a");
        CHECK(prompt.artifact_digest_display == std::string(64, 'b'));
        CHECK(prompt.selector_display == "develop\\x0aAllow");
        return ControlConsentPromptResult::Approved;
    };
    ControlBrokerConsentAuthority authority{std::move(config)};
    CHECK(authority.decide(hostile).approved);
}

TEST_CASE("broker consent authority rate limits global and per-client prompt fatigue",
          "[inspect][control][consent][security]") {
    auto now = std::chrono::steady_clock::time_point{10s};
    std::uint8_t entropy = 1;
    auto config = config_for(ControlConsentPromptResult::Approved, now, entropy);
    config.global_prompt_cooldown = 5s;
    config.client_prompt_cooldown = 30s;
    unsigned prompts = 0;
    config.prompt = [&](const ControlConsentPrompt&, std::chrono::milliseconds) {
        ++prompts;
        return ControlConsentPromptResult::Approved;
    };
    ControlBrokerConsentAuthority authority{std::move(config)};
    auto first = request();
    REQUIRE(authority.decide(first).approved);
    CHECK_FALSE(authority.decide(first).approved);
    CHECK(prompts == 1);

    now += 5s;
    auto other = first;
    other.client_peer_fingerprint = "peer-fingerprint-b";
    REQUIRE(authority.decide(other).approved);
    CHECK_FALSE(authority.decide(first).approved);
    CHECK(prompts == 2);

    now += 25s;
    REQUIRE(authority.decide(first).approved);
    CHECK(prompts == 3);

    now = std::chrono::steady_clock::time_point{10s};
    entropy = 1;
    auto timeout_config = config_for(ControlConsentPromptResult::TimedOut, now, entropy);
    timeout_config.global_prompt_cooldown = 5s;
    timeout_config.client_prompt_cooldown = 30s;
    unsigned timeout_prompts = 0;
    timeout_config.prompt = [&](const ControlConsentPrompt&, std::chrono::milliseconds timeout) {
        ++timeout_prompts;
        now += timeout;
        return ControlConsentPromptResult::TimedOut;
    };
    ControlBrokerConsentAuthority timeout_authority{std::move(timeout_config)};
    CHECK_FALSE(timeout_authority.decide(first).approved);
    now += 28s;
    CHECK_FALSE(timeout_authority.decide(first).approved);
    CHECK(timeout_prompts == 1);
    now += 2s;
    CHECK_FALSE(timeout_authority.decide(first).approved);
    CHECK(timeout_prompts == 2);
}
