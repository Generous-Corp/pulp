#pragma once

#include <pulp/inspect/control_grants.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::inspect {

enum class ControlConsentPromptResult : std::uint8_t {
    Approved,
    Denied,
    TimedOut,
    Unavailable,
};

struct ControlConsentPrompt {
    ControlGrantConsentRequest request;
    std::string client_executable_display;
    std::string client_publisher_display;
    std::string plugin_display;
    std::string instance_display;
    std::string target_publisher_display;
    std::string build_display;
    std::string artifact_digest_display;
    std::string selector_display;
};

struct ControlBrokerConsentAuthorityConfig {
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Entropy = std::function<std::optional<std::vector<std::uint8_t>>(std::size_t)>;
    using Prompt = std::function<ControlConsentPromptResult(const ControlConsentPrompt&,
                                                            std::chrono::milliseconds)>;

    std::chrono::milliseconds prompt_timeout = std::chrono::seconds(30);
    std::chrono::milliseconds global_prompt_cooldown = std::chrono::seconds(5);
    std::chrono::milliseconds client_prompt_cooldown = std::chrono::seconds(30);
    std::size_t maximum_tracked_clients = 64;
    Clock clock = [] { return std::chrono::steady_clock::now(); };
    Entropy entropy;
    /// Empty selects the broker-owned native macOS prompt. Unsupported or
    /// headless environments fail closed.
    Prompt prompt;
};

/// Process-local production consent authority. It serializes prompts, bounds
/// their lifetime, and mints a fresh decision bound to the complete
/// broker-derived request lineage only after an explicit user approval.
class ControlBrokerConsentAuthority {
  public:
    explicit ControlBrokerConsentAuthority(ControlBrokerConsentAuthorityConfig config = {});
    ~ControlBrokerConsentAuthority();
    ControlBrokerConsentAuthority(const ControlBrokerConsentAuthority&) = delete;
    ControlBrokerConsentAuthority& operator=(const ControlBrokerConsentAuthority&) = delete;

    ControlConsentDecision decide(const ControlGrantConsentRequest& request);
    /// Invalidates an in-flight result. A newly started daemon owns a new
    /// authority generation, so approvals cannot cross restart.
    void reset() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
