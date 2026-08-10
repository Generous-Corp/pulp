#include <pulp/inspect/control_consent_authority.hpp>

#include <pulp/inspect/capabilities.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && !TARGET_OS_IPHONE
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace pulp::inspect {
namespace {

void append_framed(std::string& output, std::string_view value) {
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
}

std::string canonical_request(const ControlGrantConsentRequest& request) {
    std::string value{"dev.pulp.control/consent-decision@1"};
    const auto& peer = request.client_peer;
    const auto& registration = request.registration;
    for (const auto field :
         {std::string_view{peer.user_id}, std::string_view{peer.process_start_id},
          std::string_view{peer.executable_identity}, std::string_view{peer.publisher_id},
          std::string_view{request.client_peer_fingerprint},
          std::string_view{request.grant.client_id.value},
          std::string_view{request.grant.registration_id.value},
          std::string_view{registration.broker_id.value}, std::string_view{registration.session_id},
          std::string_view{registration.instance_id}, std::string_view{registration.publication_id},
          std::string_view{registration.plugin_id}, std::string_view{registration.publisher_id},
          std::string_view{registration.manifest_digest},
          std::string_view{registration.artifact_digest},
          std::string_view{registration.consent_identity}, std::string_view{registration.build_id},
          std::string_view{request.selector_id}})
        append_framed(value, field);
    append_framed(value, std::to_string(peer.process_id));
    append_framed(value, std::to_string(static_cast<unsigned>(request.selector_kind)));
    append_framed(value, std::to_string(request.grant.ttl.count()));
    for (const auto capability : request.grant.capabilities)
        append_framed(value, capability_id(capability));
    return value;
}

std::string safe_display_token(std::string_view input) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size());
    for (const auto byte : input) {
        const auto value = static_cast<unsigned char>(byte);
        const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                          (value >= '0' && value <= '9') || value == '.' || value == '_' ||
                          value == '-' || value == '/' || value == '@' || value == ':' ||
                          value == '+';
        if (safe) {
            output.push_back(static_cast<char>(value));
        } else {
            output.append("\\x");
            output.push_back(hex[value >> 4]);
            output.push_back(hex[value & 0x0f]);
        }
    }
    return output;
}

ControlConsentPrompt make_prompt(const ControlGrantConsentRequest& request) {
    return {
        .request = request,
        .client_executable_display = safe_display_token(request.client_peer.executable_identity),
        .client_publisher_display = safe_display_token(request.client_peer.publisher_id),
        .plugin_display = safe_display_token(request.registration.plugin_id),
        .instance_display = safe_display_token(request.registration.instance_id),
        .target_publisher_display = safe_display_token(request.registration.publisher_id),
        .build_display = safe_display_token(request.registration.build_id),
        .artifact_digest_display = safe_display_token(request.registration.artifact_digest),
        .selector_display = safe_display_token(request.selector_id),
    };
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
CFStringRef make_cf_string(std::string_view text) {
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                   text.size(), kCFStringEncodingUTF8, false);
}

ControlConsentPromptResult native_prompt(const ControlConsentPrompt& prompt,
                                         std::chrono::milliseconds timeout) {
    const auto bounded_ms = std::clamp<std::int64_t>(timeout.count(), 1, 60'000);
    const std::string title = "Pulp control permission";
    const std::string selector = prompt.request.selector_kind == ControlGrantSelectorKind::Operation
                                     ? "operation " + prompt.selector_display
                                     : "profile " + prompt.selector_display;
    const std::string message = "Allow " + prompt.client_executable_display + " (publisher " +
                                prompt.client_publisher_display + ") to use " + selector + " on " +
                                prompt.plugin_display + " (instance " + prompt.instance_display +
                                ", publisher " + prompt.target_publisher_display + ", build " +
                                prompt.build_display + ", artifact sha256 " +
                                prompt.artifact_digest_display + ")?";
    auto* title_cf = make_cf_string(title);
    auto* message_cf = make_cf_string(message);
    auto* approve_cf = make_cf_string("Allow Once");
    auto* deny_cf = make_cf_string("Deny");
    if (!title_cf || !message_cf || !approve_cf || !deny_cf) {
        if (title_cf)
            CFRelease(title_cf);
        if (message_cf)
            CFRelease(message_cf);
        if (approve_cf)
            CFRelease(approve_cf);
        if (deny_cf)
            CFRelease(deny_cf);
        return ControlConsentPromptResult::Unavailable;
    }
    CFOptionFlags response = 0;
    const auto status = CFUserNotificationDisplayAlert(
        static_cast<CFTimeInterval>(bounded_ms) / 1000.0, kCFUserNotificationCautionAlertLevel,
        nullptr, nullptr, nullptr, title_cf, message_cf, deny_cf, approve_cf, nullptr, &response);
    CFRelease(title_cf);
    CFRelease(message_cf);
    CFRelease(approve_cf);
    CFRelease(deny_cf);
    if (status != 0)
        return ControlConsentPromptResult::TimedOut;
    return response == kCFUserNotificationAlternateResponse ? ControlConsentPromptResult::Approved
                                                            : ControlConsentPromptResult::Denied;
}
#else
ControlConsentPromptResult native_prompt(const ControlConsentPrompt&, std::chrono::milliseconds) {
    return ControlConsentPromptResult::Unavailable;
}
#endif

} // namespace

struct ControlBrokerConsentAuthority::Impl {
    explicit Impl(ControlBrokerConsentAuthorityConfig input) : config(std::move(input)) {
        if (!config.entropy)
            config.entropy = [](std::size_t size) { return runtime::secure_random_bytes(size); };
        if (!config.prompt)
            config.prompt = native_prompt;
        if (config.entropy)
            epoch = config.entropy(32);
    }

    ControlBrokerConsentAuthorityConfig config;
    std::mutex prompt_mutex;
    std::atomic<std::uint64_t> generation{1};
    std::optional<std::vector<std::uint8_t>> epoch;
    std::optional<std::chrono::steady_clock::time_point> last_prompt;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> client_prompts;
};

ControlBrokerConsentAuthority::ControlBrokerConsentAuthority(
    ControlBrokerConsentAuthorityConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

ControlBrokerConsentAuthority::~ControlBrokerConsentAuthority() = default;

ControlConsentDecision
ControlBrokerConsentAuthority::decide(const ControlGrantConsentRequest& request) {
    if (!impl_->config.clock || !impl_->config.entropy || !impl_->config.prompt || !impl_->epoch ||
        impl_->epoch->size() != 32 ||
        impl_->config.prompt_timeout <= std::chrono::milliseconds::zero() ||
        impl_->config.global_prompt_cooldown < std::chrono::milliseconds::zero() ||
        impl_->config.client_prompt_cooldown < std::chrono::milliseconds::zero() ||
        impl_->config.maximum_tracked_clients == 0)
        return {};
    std::unique_lock lock(impl_->prompt_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return {};
    const auto generation = impl_->generation.load(std::memory_order_acquire);
    const auto started = impl_->config.clock();
    if ((impl_->last_prompt &&
         started - *impl_->last_prompt < impl_->config.global_prompt_cooldown) ||
        (impl_->client_prompts.contains(request.client_peer_fingerprint) &&
         started - impl_->client_prompts.at(request.client_peer_fingerprint) <
             impl_->config.client_prompt_cooldown))
        return {};
    if (impl_->config.prompt_timeout > std::chrono::steady_clock::time_point::max() - started)
        return {};
    const auto deadline = started + impl_->config.prompt_timeout;
    const auto prompt_result =
        impl_->config.prompt(make_prompt(request), impl_->config.prompt_timeout);
    const auto completed = impl_->config.clock();
    impl_->last_prompt = completed;
    if (!impl_->client_prompts.contains(request.client_peer_fingerprint) &&
        impl_->client_prompts.size() >= impl_->config.maximum_tracked_clients)
        impl_->client_prompts.erase(impl_->client_prompts.begin());
    impl_->client_prompts[request.client_peer_fingerprint] = completed;
    if (prompt_result != ControlConsentPromptResult::Approved || completed >= deadline ||
        impl_->generation.load(std::memory_order_acquire) != generation)
        return {};
    const auto nonce = impl_->config.entropy(32);
    if (!nonce || nonce->size() != 32)
        return {};
    auto canonical = canonical_request(request);
    canonical.append(reinterpret_cast<const char*>(impl_->epoch->data()), impl_->epoch->size());
    canonical.append(reinterpret_cast<const char*>(nonce->data()), nonce->size());
    auto decision_id = "broker-prompt-" + runtime::sha256_hex(canonical);
    // Entropy is a trusted callback but still sits inside the bounded decision
    // lifetime. This final check is the approval linearization point.
    if (impl_->config.clock() >= deadline ||
        impl_->generation.load(std::memory_order_acquire) != generation)
        return {};
    return {.approved = true,
            .authority = ControlConsentAuthority::BrokerUserPrompt,
            .decision_id = std::move(decision_id),
            .expires_at = deadline};
}

void ControlBrokerConsentAuthority::reset() noexcept {
    impl_->generation.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace pulp::inspect
