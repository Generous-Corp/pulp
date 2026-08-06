#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_operations.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using namespace pulp::inspect;
using namespace std::chrono_literals;

namespace {

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto random = pulp::runtime::secure_random_bytes(8);
        REQUIRE(random.has_value());
        path = std::filesystem::temp_directory_path() /
               ("pulp-control-operations-" + pulp::runtime::hex_encode(*random));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

ControlOperationBinding
binding(std::string request = R"({"parameter_id":7,"normalized_value":0.5})",
        std::string key = "request-1", std::string grant = "grant-a") {
    ControlOperationBinding result;
    static_cast<ControlAuthorityBinding&>(result) = {
        .broker_id = ControlBrokerId{"broker-a"},
        .client_principal = "peer:client-a",
        .client_id = ControlClientId{"client-a"},
        .registration_id = ControlRegistrationId{"registration-generation-a"},
        .grant_id = ControlGrantId{std::move(grant)},
        .session_id = "session-a",
        .instance_id = "instance-a",
        .publication_id = "publication-a",
        .instance_generation = "instance-generation-a",
        .capability = InspectorCapability::StateWrite,
        .operation_id = "dev.pulp.state/parameter-set@1",
        .operation_version = 1,
        .consent_decision_id = "consent-a",
        .manifest_digest = std::string(64, 'a'),
        .producer_artifact_digest = std::string(64, 'b'),
        .deadline_unix_ms = 200000,
        .expected_state_generation = 7,
    };
    result.request_id = "request-wire-" + key;
    result.idempotency_key = std::move(key);
    result.canonical_request_hash = pulp::runtime::sha256_hex(request);
    return result;
}

ControlOperationStore store_at(const std::filesystem::path& path,
                               std::chrono::system_clock::time_point* now = nullptr,
                               std::size_t capacity = 64, std::size_t active_per_client = 64,
                               std::size_t active_per_registration_instance = 64,
                               std::chrono::milliseconds replay_window = std::chrono::hours{24},
                               std::chrono::milliseconds retention = std::chrono::hours{24 * 7},
                               std::size_t maximum_receipt_bytes = 64u * 1024u) {
    ControlOperationStoreConfig config;
    config.directory = path;
    config.max_receipts = capacity;
    config.max_active_receipts_per_client = active_per_client;
    config.max_active_receipts_per_registration_instance = active_per_registration_instance;
    config.max_receipt_bytes = maximum_receipt_bytes;
    config.replay_window = replay_window;
    config.retention = retention;
    if (now) {
        return ControlOperationStore{std::move(config), [now] { return *now; }};
    }
    return ControlOperationStore{std::move(config)};
}

#ifdef _WIN32
bool owner_private_windows_path(const std::filesystem::path& path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD token_bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &token_bytes);
    std::vector<std::uint8_t> token_storage(token_bytes);
    const bool token_read =
        token_bytes != 0 &&
        GetTokenInformation(token, TokenUser, token_storage.data(), token_bytes, &token_bytes) != 0;
    CloseHandle(token);
    if (!token_read)
        return false;
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token_storage.data());

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto status =
        GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                              nullptr, &dacl, nullptr, &descriptor);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool control_read =
        descriptor && GetSecurityDescriptorControl(descriptor, &control, &revision);
    void* raw_ace = nullptr;
    const bool one_ace = dacl && dacl->AceCount == 1 && GetAce(dacl, 0, &raw_ace);
    const auto* header = one_ace ? static_cast<const ACE_HEADER*>(raw_ace) : nullptr;
    const auto* allowed = header && header->AceType == ACCESS_ALLOWED_ACE_TYPE
                              ? static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace)
                              : nullptr;
    const bool secure = status == ERROR_SUCCESS && owner && EqualSid(owner, token_user->User.Sid) &&
                        control_read && (control & SE_DACL_PROTECTED) != 0 && allowed &&
                        EqualSid(const_cast<DWORD*>(&allowed->SidStart), token_user->User.Sid);
    if (descriptor)
        LocalFree(descriptor);
    return secure;
}

bool grant_world_read(const std::filesystem::path& path) {
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> world_storage{};
    DWORD world_bytes = static_cast<DWORD>(world_storage.size());
    if (!CreateWellKnownSid(WinWorldSid, nullptr, world_storage.data(), &world_bytes))
        return false;
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto read_status = GetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
        nullptr, &current, nullptr, &descriptor);
    if (read_status != ERROR_SUCCESS || !current) {
        if (descriptor)
            LocalFree(descriptor);
        return false;
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<wchar_t*>(world_storage.data());
    PACL updated = nullptr;
    const auto acl_status = SetEntriesInAclW(1, &access, current, &updated);
    const auto write_status =
        acl_status == ERROR_SUCCESS
            ? SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr)
            : acl_status;
    if (updated)
        LocalFree(updated);
    LocalFree(descriptor);
    return write_status == ERROR_SUCCESS;
}
#endif

} // namespace

TEST_CASE("operation receipt transitions preserve admitted running terminal order",
          "[inspect][control][operations][receipt]") {
    TemporaryDirectory temporary;
    auto now = std::chrono::system_clock::time_point{123456ms};
    auto store = store_at(temporary.path, &now);
    REQUIRE(store.open().status == ControlOperationStoreStatus::Opened);

    const auto admitted = store.admit(binding());
    REQUIRE(admitted.status == ControlOperationStoreStatus::Admitted);
    REQUIRE(admitted.receipt.has_value());
    CHECK(admitted.receipt->state == ControlReceiptState::Admitted);
    CHECK(admitted.receipt->created_at_unix_ms == 123456);
    CHECK(admitted.receipt->updated_at_unix_ms == 123456);

    const auto illegal =
        store.transition(admitted.receipt->receipt_id, ControlReceiptState::Admitted,
                         ControlReceiptState::Completed);
    CHECK(illegal.status == ControlOperationStoreStatus::InvalidTransition);
    CHECK(store.receipt(admitted.receipt->receipt_id)->state == ControlReceiptState::Admitted);

    now += 10ms;
    const auto running = store.begin(admitted.receipt->receipt_id);
    REQUIRE(running.status == ControlOperationStoreStatus::Transitioned);
    REQUIRE(running.receipt.has_value());
    CHECK(running.receipt->updated_at_unix_ms == 123466);

    now += 10ms;
    ControlOperationResult result;
    result.explanation = "state applied";
    result.detail_json = R"({"state_generation":8})";
    result.artifacts.push_back(ControlArtifactHandle{"artifact-a", "application/json", 42});
    result.evidence_ids.push_back("evidence-generation-8");
    const auto completed =
        store.transition(admitted.receipt->receipt_id, ControlReceiptState::Running,
                         ControlReceiptState::Completed, std::move(result));
    REQUIRE(completed.status == ControlOperationStoreStatus::Transitioned);
    REQUIRE(completed.receipt.has_value());
    CHECK(control_receipt_state_is_terminal(completed.receipt->state));
    CHECK_FALSE(completed.receipt->result.result_code.has_value());
    CHECK(completed.receipt->result.detail_json == R"({"state_generation": 8})");
    REQUIRE(completed.receipt->result.artifacts.size() == 1);
    CHECK(completed.receipt->result.artifacts.front().artifact_id == "artifact-a");
    CHECK(completed.receipt->result.evidence_ids ==
          std::vector<std::string>{"evidence-generation-8"});

    CHECK(store
              .transition(admitted.receipt->receipt_id, ControlReceiptState::Completed,
                          ControlReceiptState::Running)
              .status == ControlOperationStoreStatus::InvalidTransition);
}

TEST_CASE("durable receipt fields enforce the public wire boundaries",
          "[inspect][control][operations][receipt][limits]") {
    const auto transition_status = [](ControlOperationResult result) {
        TemporaryDirectory temporary;
        auto now = std::chrono::system_clock::time_point{123456ms};
        auto store = store_at(temporary.path, &now);
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt);
        REQUIRE(store.begin(admitted.receipt->receipt_id).succeeded());
        return store
            .transition(admitted.receipt->receipt_id, ControlReceiptState::Running,
                        ControlReceiptState::Completed, std::move(result))
            .status;
    };

    ControlOperationResult explanation_at_limit;
    explanation_at_limit.explanation = std::string(kControlReceiptMaximumExplanationBytes, 'x');
    CHECK(transition_status(std::move(explanation_at_limit)) ==
          ControlOperationStoreStatus::Transitioned);
    ControlOperationResult explanation_over_limit;
    explanation_over_limit.explanation =
        std::string(kControlReceiptMaximumExplanationBytes + 1, 'x');
    CHECK(transition_status(std::move(explanation_over_limit)) ==
          ControlOperationStoreStatus::InvalidRequest);
    ControlOperationResult explanation_with_nul;
    explanation_with_nul.explanation = std::string{"x\0y", 3};
    CHECK(transition_status(std::move(explanation_with_nul)) ==
          ControlOperationStoreStatus::InvalidRequest);

    ControlOperationResult media_at_limit;
    media_at_limit.artifacts.push_back(
        {"artifact-a", std::string(kControlReceiptMaximumArtifactMediaTypeBytes, 'm'), 1});
    CHECK(transition_status(std::move(media_at_limit)) ==
          ControlOperationStoreStatus::Transitioned);
    ControlOperationResult media_over_limit;
    media_over_limit.artifacts.push_back(
        {"artifact-a", std::string(kControlReceiptMaximumArtifactMediaTypeBytes + 1, 'm'), 1});
    CHECK(transition_status(std::move(media_over_limit)) ==
          ControlOperationStoreStatus::InvalidRequest);
    ControlOperationResult invalid_media_token;
    invalid_media_token.artifacts.push_back({"artifact-a", "application / json", 1});
    CHECK(transition_status(std::move(invalid_media_token)) ==
          ControlOperationStoreStatus::InvalidRequest);

    ControlOperationResult artifact_id_over_limit;
    artifact_id_over_limit.artifacts.push_back(
        {std::string(kControlReceiptMaximumArtifactIdBytes + 1, 'a'), "application/octet-stream",
         1});
    CHECK(transition_status(std::move(artifact_id_over_limit)) ==
          ControlOperationStoreStatus::InvalidRequest);
    ControlOperationResult evidence_over_limit;
    evidence_over_limit.evidence_ids.push_back(
        std::string(kControlReceiptMaximumEvidenceIdBytes + 1, 'e'));
    CHECK(transition_status(std::move(evidence_over_limit)) ==
          ControlOperationStoreStatus::InvalidRequest);

    TemporaryDirectory request_limit;
    auto store = store_at(request_limit.path);
    REQUIRE(store.open().succeeded());
    auto maximum_request = binding();
    maximum_request.request_id = std::string(kControlReceiptMaximumRequestIdBytes, 'r');
    CHECK(store.admit(std::move(maximum_request)).status == ControlOperationStoreStatus::Admitted);
    auto oversized_request = binding("{}", "oversized-request");
    oversized_request.request_id = std::string(kControlReceiptMaximumRequestIdBytes + 1, 'r');
    CHECK(store.admit(std::move(oversized_request)).status ==
          ControlOperationStoreStatus::InvalidRequest);

    const auto cancellable = store.admit(binding("{}", "cancellation-limit"));
    REQUIRE(cancellable.receipt);
    CHECK(store
              .request_cancellation(cancellable.receipt->receipt_id,
                                    std::string(kControlReceiptMaximumCancellationReasonBytes, 'c'))
              .status == ControlOperationStoreStatus::CancellationRequested);
    const auto cancellation_over = store.admit(binding("{}", "cancellation-over-limit"));
    REQUIRE(cancellation_over.receipt);
    CHECK(store
              .request_cancellation(
                  cancellation_over.receipt->receipt_id,
                  std::string(kControlReceiptMaximumCancellationReasonBytes + 1, 'c'))
              .status == ControlOperationStoreStatus::InvalidRequest);
}

TEST_CASE("receipt commits reject invalid UTF-8 without poisoning durable state",
          "[inspect][control][operations][receipt][persistence][security]") {
    const auto read_file = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto rejected_transition_survives_restart = [&](auto mutate) {
        TemporaryDirectory temporary;
        auto now = std::chrono::system_clock::time_point{123456ms};
        ControlReceiptId receipt_id;
        std::string committed_before;
        {
            auto store = store_at(temporary.path, &now);
            REQUIRE(store.open().succeeded());
            const auto admitted = store.admit(binding());
            REQUIRE(admitted.receipt);
            receipt_id = admitted.receipt->receipt_id;
            REQUIRE(store.begin(receipt_id).succeeded());
            const auto receipt_path = temporary.path / (receipt_id.value + ".json");
            committed_before = read_file(receipt_path);

            ControlOperationResult result;
            result.result_code = ControlResultCode::InternalError;
            mutate(result);
            CHECK(store
                      .transition(receipt_id, ControlReceiptState::Running,
                                  ControlReceiptState::Failed, std::move(result))
                      .status == ControlOperationStoreStatus::InvalidRequest);
            REQUIRE(store.receipt(receipt_id));
            CHECK(store.receipt(receipt_id)->state == ControlReceiptState::Running);
            CHECK(read_file(receipt_path) == committed_before);
        }

        auto reopened = store_at(temporary.path, &now);
        REQUIRE(reopened.open().succeeded());
        REQUIRE(reopened.receipt(receipt_id));
        CHECK(reopened.receipt(receipt_id)->state == ControlReceiptState::UnknownNeedsRefresh);
    };

    SECTION("explanation") {
        rejected_transition_survives_restart([](ControlOperationResult& result) {
            result.explanation = std::string(1, static_cast<char>(0xff));
        });
    }
    SECTION("result cancellation reason") {
        rejected_transition_survives_restart([](ControlOperationResult& result) {
            result.cancellation_reason = std::string(1, static_cast<char>(0xc3));
        });
    }
    SECTION("evidence id") {
        rejected_transition_survives_restart([](ControlOperationResult& result) {
            result.evidence_ids.emplace_back("evidence-");
            result.evidence_ids.back().append("\xed\xa0\x80", 3);
        });
    }

    SECTION("cancellation request") {
        TemporaryDirectory temporary;
        auto store = store_at(temporary.path);
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt);
        CHECK(store
                  .request_cancellation(admitted.receipt->receipt_id,
                                        std::string(1, static_cast<char>(0xff)))
                  .status == ControlOperationStoreStatus::InvalidRequest);
        REQUIRE(store.receipt(admitted.receipt->receipt_id));
        CHECK_FALSE(store.receipt(admitted.receipt->receipt_id)->cancellation_requested);
    }

    SECTION("binding") {
        TemporaryDirectory temporary;
        auto store = store_at(temporary.path);
        REQUIRE(store.open().succeeded());
        auto invalid = binding();
        invalid.session_id = std::string(1, static_cast<char>(0xff));
        CHECK(store.admit(std::move(invalid)).status ==
              ControlOperationStoreStatus::InvalidRequest);
        CHECK(store.receipts().empty());
    }
}

TEST_CASE("receipt commits accept valid UTF-8 at exact public byte boundaries",
          "[inspect][control][operations][receipt][persistence][limits]") {
    const auto repeat_to_bytes = [](std::string_view scalar, std::size_t bytes) {
        REQUIRE(bytes % scalar.size() == 0);
        std::string result;
        result.reserve(bytes);
        while (result.size() < bytes)
            result.append(scalar);
        return result;
    };
    TemporaryDirectory temporary;
    auto now = std::chrono::system_clock::time_point{123456ms};
    ControlReceiptId receipt_id;
    ControlOperationResult result;
    result.explanation = repeat_to_bytes("\xc3\xa9", kControlReceiptMaximumExplanationBytes);
    result.cancellation_reason =
        repeat_to_bytes("\xc3\xa9", kControlReceiptMaximumCancellationReasonBytes);
    result.evidence_ids.push_back(
        repeat_to_bytes("\xf0\x9f\x8e\x9b", kControlReceiptMaximumEvidenceIdBytes));
    {
        auto store = store_at(temporary.path, &now);
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt);
        receipt_id = admitted.receipt->receipt_id;
        REQUIRE(store.begin(receipt_id).succeeded());
        const auto completed = store.transition(receipt_id, ControlReceiptState::Running,
                                                ControlReceiptState::Completed, result);
        REQUIRE(completed.succeeded());
        REQUIRE(completed.receipt);
        CHECK(completed.receipt->result.explanation == result.explanation);
        CHECK(completed.receipt->result.cancellation_reason == result.cancellation_reason);
        CHECK(completed.receipt->result.evidence_ids == result.evidence_ids);
    }

    auto reopened = store_at(temporary.path, &now);
    REQUIRE(reopened.open().succeeded());
    REQUIRE(reopened.receipt(receipt_id));
    CHECK(reopened.receipt(receipt_id)->result.explanation == result.explanation);
    CHECK(reopened.receipt(receipt_id)->result.cancellation_reason == result.cancellation_reason);
    CHECK(reopened.receipt(receipt_id)->result.evidence_ids == result.evidence_ids);
}

TEST_CASE("idempotent admission replays exact content and conflicts on drift",
          "[inspect][control][operations][idempotency]") {
    TemporaryDirectory temporary;
    auto store = store_at(temporary.path);
    REQUIRE(store.open().succeeded());

    const auto first = store.admit(binding());
    REQUIRE(first.receipt.has_value());

    const auto replay = store.admit(binding());
    REQUIRE(replay.status == ControlOperationStoreStatus::Replay);
    REQUIRE(replay.receipt.has_value());
    CHECK(replay.receipt->receipt_id == first.receipt->receipt_id);
    CHECK(store.receipts().size() == 1);

    auto transport_retry = binding();
    transport_retry.request_id = "request-wire-retry";
    transport_retry.deadline_unix_ms += 10'000;
    const auto retried = store.admit(std::move(transport_retry));
    REQUIRE(retried.status == ControlOperationStoreStatus::Replay);
    REQUIRE(retried.receipt);
    CHECK(retried.receipt->binding.request_id == "request-wire-request-1");
    CHECK(retried.receipt->binding.deadline_unix_ms == 200000);

    const auto conflict = store.admit(binding(R"({"parameter_id":7,"normalized_value":0.75})"));
    CHECK(conflict.status == ControlOperationStoreStatus::IdempotencyConflict);
    REQUIRE(conflict.receipt.has_value());
    CHECK(conflict.receipt->receipt_id == first.receipt->receipt_id);
    CHECK(store.receipts().size() == 1);

    auto generation_drift = binding();
    generation_drift.expected_state_generation = 8;
    CHECK(store.admit(std::move(generation_drift)).status ==
          ControlOperationStoreStatus::IdempotencyConflict);

    auto other_grant_binding =
        binding(R"({"parameter_id":7,"normalized_value":0.75})", "request-1", "grant-b");
    other_grant_binding.request_id = "request-wire-2";
    const auto other_grant = store.admit(std::move(other_grant_binding));
    CHECK(other_grant.status == ControlOperationStoreStatus::Admitted);
    CHECK(store.receipts().size() == 2);
}

TEST_CASE("idempotent transport retries survive durable reconnect",
          "[inspect][control][operations][idempotency][persistence]") {
    TemporaryDirectory temporary;
    auto now = std::chrono::system_clock::time_point{1000ms};
    ControlReceiptId receipt_id;
    {
        auto store = store_at(temporary.path, &now);
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt);
        receipt_id = admitted.receipt->receipt_id;
    }

    auto reopened = store_at(temporary.path, &now);
    REQUIRE(reopened.open().succeeded());
    auto retry = binding();
    retry.request_id = "request-after-reconnect";
    retry.deadline_unix_ms += 60'000;
    const auto replay = reopened.admit(std::move(retry));
    REQUIRE(replay.status == ControlOperationStoreStatus::Replay);
    REQUIRE(replay.receipt);
    CHECK(replay.receipt->receipt_id == receipt_id);
    CHECK(replay.receipt->binding.request_id == "request-wire-request-1");
    CHECK(replay.receipt->binding.deadline_unix_ms == 200000);

    auto changed_content = binding(R"({"parameter_id":7,"normalized_value":0.75})");
    changed_content.request_id = "request-after-reconnect-2";
    changed_content.deadline_unix_ms += 60'000;
    CHECK(reopened.admit(std::move(changed_content)).status ==
          ControlOperationStoreStatus::IdempotencyConflict);
}

TEST_CASE("simultaneous duplicate admissions create one durable receipt",
          "[inspect][control][operations][idempotency][concurrency]") {
    TemporaryDirectory temporary;
    auto store = store_at(temporary.path);
    REQUIRE(store.open().succeeded());

    std::mutex results_mutex;
    std::vector<ControlOperationStoreResult> results;
    std::vector<std::thread> callers;
    for (int index = 0; index < 8; ++index) {
        callers.emplace_back([&] {
            auto result = store.admit(binding());
            std::lock_guard lock(results_mutex);
            results.push_back(std::move(result));
        });
    }
    for (auto& caller : callers)
        caller.join();

    REQUIRE(results.size() == 8);
    const auto admitted_count =
        std::count_if(results.begin(), results.end(), [](const auto& result) {
            return result.status == ControlOperationStoreStatus::Admitted;
        });
    const auto replay_count = std::count_if(results.begin(), results.end(), [](const auto& result) {
        return result.status == ControlOperationStoreStatus::Replay;
    });
    CHECK(admitted_count == 1);
    CHECK(replay_count == 7);
    REQUIRE(store.receipts().size() == 1);
    for (const auto& result : results) {
        REQUIRE(result.receipt.has_value());
        CHECK(result.receipt->receipt_id == store.receipts().front().receipt_id);
    }
}

TEST_CASE("active receipt quotas enforce client and registered-instance boundaries",
          "[inspect][control][operations][quota]") {
    SECTION("client quota blocks new work but never replay") {
        TemporaryDirectory temporary;
        auto store = store_at(temporary.path, nullptr, 64, 2, 64);
        REQUIRE(store.open().succeeded());

        auto first_binding = binding("{}", "client-work-1");
        first_binding.registration_id = ControlRegistrationId{"registration-1"};
        first_binding.instance_id = "instance-1";
        const auto first = store.admit(first_binding);
        REQUIRE(first.status == ControlOperationStoreStatus::Admitted);

        auto second_binding = binding("{}", "client-work-2");
        second_binding.registration_id = ControlRegistrationId{"registration-2"};
        second_binding.instance_id = "instance-2";
        REQUIRE(store.admit(second_binding).status == ControlOperationStoreStatus::Admitted);

        auto over_binding = binding("{}", "client-work-3");
        over_binding.registration_id = ControlRegistrationId{"registration-3"};
        over_binding.instance_id = "instance-3";
        const auto over = store.admit(std::move(over_binding));
        CHECK(over.status == ControlOperationStoreStatus::ResourceExhausted);
        CHECK(over.error == "active receipt client quota is exhausted");
        CHECK(store.receipts().size() == 2);

        const auto replay = store.admit(std::move(first_binding));
        REQUIRE(replay.status == ControlOperationStoreStatus::Replay);
        REQUIRE(replay.receipt.has_value());
        CHECK(replay.receipt->receipt_id == first.receipt->receipt_id);

        auto other_client = binding("{}", "other-client-work");
        other_client.client_id = ControlClientId{"client-b"};
        other_client.client_principal = "peer:client-b";
        other_client.registration_id = ControlRegistrationId{"registration-4"};
        other_client.instance_id = "instance-4";
        CHECK(store.admit(std::move(other_client)).status == ControlOperationStoreStatus::Admitted);
    }

    SECTION("registered instance quota is isolated and terminal work frees a slot") {
        TemporaryDirectory temporary;
        auto store = store_at(temporary.path, nullptr, 64, 64, 2);
        REQUIRE(store.open().succeeded());

        auto first_binding = binding("{}", "instance-work-1");
        const auto first = store.admit(std::move(first_binding));
        REQUIRE(first.receipt.has_value());

        auto second_binding = binding("{}", "instance-work-2");
        second_binding.client_id = ControlClientId{"client-b"};
        second_binding.client_principal = "peer:client-b";
        const auto second = store.admit(std::move(second_binding));
        REQUIRE(second.status == ControlOperationStoreStatus::Admitted);

        auto over_binding = binding("{}", "instance-work-3");
        over_binding.client_id = ControlClientId{"client-c"};
        over_binding.client_principal = "peer:client-c";
        const auto over = store.admit(over_binding);
        CHECK(over.status == ControlOperationStoreStatus::ResourceExhausted);
        CHECK(over.error == "active receipt registration/instance quota is exhausted");
        CHECK(store.receipts().size() == 2);

        ControlOperationResult failed;
        failed.result_code = ControlResultCode::InternalError;
        REQUIRE(store
                    .transition(first.receipt->receipt_id, ControlReceiptState::Admitted,
                                ControlReceiptState::Failed, std::move(failed))
                    .succeeded());
        CHECK(store.admit(std::move(over_binding)).status == ControlOperationStoreStatus::Admitted);

        auto other_instance = binding("{}", "other-instance-work");
        other_instance.registration_id = ControlRegistrationId{"registration-b"};
        other_instance.instance_id = "instance-b";
        CHECK(store.admit(std::move(other_instance)).status ==
              ControlOperationStoreStatus::Admitted);
    }
}

TEST_CASE("concurrent admissions cannot oversubscribe active quotas",
          "[inspect][control][operations][quota][concurrency]") {
    TemporaryDirectory temporary;
    auto store = store_at(temporary.path, nullptr, 64, 3, 3);
    REQUIRE(store.open().succeeded());

    std::mutex results_mutex;
    std::vector<ControlOperationStoreResult> results;
    std::vector<std::thread> callers;
    for (int index = 0; index < 12; ++index) {
        callers.emplace_back([&, index] {
            auto candidate = binding("{}", "concurrent-work-" + std::to_string(index));
            auto result = store.admit(std::move(candidate));
            std::lock_guard lock(results_mutex);
            results.push_back(std::move(result));
        });
    }
    for (auto& caller : callers)
        caller.join();

    REQUIRE(results.size() == 12);
    CHECK(std::count_if(results.begin(), results.end(), [](const auto& result) {
              return result.status == ControlOperationStoreStatus::Admitted;
          }) == 3);
    CHECK(std::count_if(results.begin(), results.end(), [](const auto& result) {
              return result.status == ControlOperationStoreStatus::ResourceExhausted;
          }) == 9);
    for (const auto& result : results) {
        if (result.status == ControlOperationStoreStatus::Admitted)
            continue;
        CHECK(result.status == ControlOperationStoreStatus::ResourceExhausted);
        CHECK(result.error == "active receipt client quota is exhausted");
    }
    CHECK(store.receipts().size() == 3);
}

TEST_CASE("operation begin resolves cancellation and deadline before running",
          "[inspect][control][operations][cancellation][deadline][concurrency]") {
    SECTION("cancellation already durable wins before execution") {
        TemporaryDirectory temporary;
        auto now = std::chrono::system_clock::time_point{1000ms};
        auto store = store_at(temporary.path, &now);
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt);
        REQUIRE(store.request_cancellation(admitted.receipt->receipt_id, "cancel-before-start")
                    .succeeded());

        const auto begun = store.begin(admitted.receipt->receipt_id);
        REQUIRE(begun.status == ControlOperationStoreStatus::Transitioned);
        REQUIRE(begun.receipt);
        CHECK(begun.receipt->state == ControlReceiptState::Cancelled);
        CHECK(begun.receipt->result.result_code == ControlResultCode::Cancelled);
        CHECK(begun.receipt->result.cancellation_reason == "cancel-before-start");
    }

    SECTION("the receipt deadline is checked inside the start transition") {
        TemporaryDirectory temporary;
        auto now = std::chrono::system_clock::time_point{1000ms};
        auto store = store_at(temporary.path, &now);
        REQUIRE(store.open().succeeded());
        auto expiring = binding();
        expiring.deadline_unix_ms = 1001;
        const auto admitted = store.admit(std::move(expiring));
        REQUIRE(admitted.receipt);
        now += 1ms;

        const auto begun = store.begin(admitted.receipt->receipt_id);
        REQUIRE(begun.status == ControlOperationStoreStatus::Transitioned);
        REQUIRE(begun.receipt);
        CHECK(begun.receipt->state == ControlReceiptState::Failed);
        CHECK(begun.receipt->result.result_code == ControlResultCode::DeadlineExceeded);
    }

    SECTION("simultaneous begin and cancellation serialize to one legal order") {
        TemporaryDirectory temporary;
        auto now = std::chrono::system_clock::time_point{1000ms};
        auto store = store_at(temporary.path, &now, 64, 1, 1);
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt);

        std::barrier ready{3};
        ControlOperationStoreResult begun;
        ControlOperationStoreResult cancelled;
        std::thread starter([&] {
            ready.arrive_and_wait();
            begun = store.begin(admitted.receipt->receipt_id);
        });
        std::thread canceller([&] {
            ready.arrive_and_wait();
            cancelled =
                store.request_cancellation(admitted.receipt->receipt_id, "racing-cancellation");
        });
        ready.arrive_and_wait();
        starter.join();
        canceller.join();

        REQUIRE(begun.receipt);
        REQUIRE(cancelled.receipt);
        const auto final = store.receipt(admitted.receipt->receipt_id);
        REQUIRE(final);
        if (begun.receipt->state == ControlReceiptState::Cancelled) {
            CHECK(cancelled.status == ControlOperationStoreStatus::CancellationRequested);
            CHECK(final->state == ControlReceiptState::Cancelled);
        } else {
            CHECK(begun.receipt->state == ControlReceiptState::Running);
            CHECK(cancelled.status == ControlOperationStoreStatus::CancellationRequested);
            CHECK(final->state == ControlReceiptState::Running);
            CHECK(final->cancellation_requested);
        }
    }
}

TEST_CASE("client request ids remain unique while receipts are retained",
          "[inspect][control][operations][request-id][retention]") {
    TemporaryDirectory temporary;
    auto now = std::chrono::system_clock::time_point{1000ms};
    auto store = store_at(temporary.path, &now, 64, 64, 64, 100ms, 1000ms);
    REQUIRE(store.open().succeeded());
    const auto first = store.admit(binding());
    REQUIRE(first.receipt);

    auto duplicate = binding("{}", "different-idempotency-key", "grant-b");
    duplicate.request_id = "request-wire-request-1";
    const auto conflict = store.admit(duplicate);
    CHECK(conflict.status == ControlOperationStoreStatus::RequestIdConflict);
    CHECK(control_operation_store_status_id(conflict.status) == "request-id-conflict");
    REQUIRE(conflict.receipt);
    CHECK(conflict.receipt->receipt_id == first.receipt->receipt_id);

    ControlOperationResult failed;
    failed.result_code = ControlResultCode::InternalError;
    REQUIRE(store
                .transition(first.receipt->receipt_id, ControlReceiptState::Admitted,
                            ControlReceiptState::Failed, std::move(failed))
                .succeeded());
    CHECK(store.admit(duplicate).status == ControlOperationStoreStatus::RequestIdConflict);

    now += 1000ms;
    CHECK(store.admit(std::move(duplicate)).status == ControlOperationStoreStatus::Admitted);
}

TEST_CASE("terminal replay window expires without admitting a second operation",
          "[inspect][control][operations][replay][retention]") {
    TemporaryDirectory temporary;
    auto now = std::chrono::system_clock::time_point{1000ms};
    auto store = store_at(temporary.path, &now, 64, 64, 64, 100ms, 1000ms);
    REQUIRE(store.open().succeeded());
    const auto admitted = store.admit(binding());
    REQUIRE(admitted.receipt.has_value());

    ControlOperationResult failed;
    failed.result_code = ControlResultCode::InternalError;
    REQUIRE(store
                .transition(admitted.receipt->receipt_id, ControlReceiptState::Admitted,
                            ControlReceiptState::Failed, std::move(failed))
                .succeeded());

    now += 99ms;
    CHECK(store.admit(binding()).status == ControlOperationStoreStatus::Replay);

    now += 1ms;
    const auto expired = store.admit(binding());
    CHECK(expired.status == ControlOperationStoreStatus::ReplayWindowExpired);
    CHECK_FALSE(expired.succeeded());
    CHECK(expired.error == "terminal receipt is outside the replay window");
    REQUIRE(expired.receipt.has_value());
    CHECK(expired.receipt->receipt_id == admitted.receipt->receipt_id);
    CHECK(control_receipt_state_is_terminal(expired.receipt->state));
    CHECK(store.receipts().size() == 1);

    auto reopened = store_at(temporary.path, &now, 64, 64, 64, 100ms, 1000ms);
    REQUIRE(reopened.open().succeeded());
    const auto reopened_expired = reopened.admit(binding());
    CHECK(reopened_expired.status == ControlOperationStoreStatus::ReplayWindowExpired);
    REQUIRE(reopened_expired.receipt.has_value());
    CHECK(reopened_expired.receipt->receipt_id == admitted.receipt->receipt_id);
    CHECK(reopened_expired.error == "terminal receipt is outside the replay window");
    CHECK(reopened.receipts().size() == 1);
}

TEST_CASE("terminal retention prunes durably at the boundary",
          "[inspect][control][operations][retention][persistence]") {
    SECTION("open prunes terminal receipts but not one millisecond early") {
        TemporaryDirectory temporary;
        auto now = std::chrono::system_clock::time_point{1000ms};
        ControlReceiptId receipt_id;
        std::filesystem::path receipt_path;
        {
            auto store = store_at(temporary.path, &now, 64, 64, 64, 100ms, 1000ms);
            REQUIRE(store.open().succeeded());
            const auto admitted = store.admit(binding());
            REQUIRE(admitted.receipt.has_value());
            receipt_id = admitted.receipt->receipt_id;
            receipt_path = temporary.path / (receipt_id.value + ".json");
            ControlOperationResult failed;
            failed.result_code = ControlResultCode::InternalError;
            REQUIRE(store
                        .transition(receipt_id, ControlReceiptState::Admitted,
                                    ControlReceiptState::Failed, std::move(failed))
                        .succeeded());
        }

        now += 999ms;
        {
            auto retained = store_at(temporary.path, &now, 64, 64, 64, 100ms, 1000ms);
            REQUIRE(retained.open().succeeded());
            CHECK(retained.receipt(receipt_id).has_value());
            CHECK(std::filesystem::exists(receipt_path));
        }

        now += 1ms;
        auto pruned = store_at(temporary.path, &now, 64, 64, 64, 100ms, 1000ms);
        REQUIRE(pruned.open().succeeded());
        CHECK_FALSE(pruned.receipt(receipt_id).has_value());
        CHECK_FALSE(std::filesystem::exists(receipt_path));
    }

    SECTION("admit prunes expired terminal receipts before new work") {
        TemporaryDirectory temporary;
        auto now = std::chrono::system_clock::time_point{1000ms};
        auto store = store_at(temporary.path, &now, 64, 64, 64, 100ms, 1000ms);
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt.has_value());
        const auto old_path = temporary.path / (admitted.receipt->receipt_id.value + ".json");
        ControlOperationResult failed;
        failed.result_code = ControlResultCode::InternalError;
        REQUIRE(store
                    .transition(admitted.receipt->receipt_id, ControlReceiptState::Admitted,
                                ControlReceiptState::Failed, std::move(failed))
                    .succeeded());

        now += 1000ms;
        const auto next = store.admit(binding("{}", "next-work"));
        CHECK(next.status == ControlOperationStoreStatus::Admitted);
        CHECK_FALSE(std::filesystem::exists(old_path));
        CHECK(store.receipts().size() == 1);
    }

    SECTION("reopen terminalizes admitted receipts before releasing quota") {
        TemporaryDirectory temporary;
        auto now = std::chrono::system_clock::time_point{1000ms};
        ControlReceiptId receipt_id;
        {
            auto store = store_at(temporary.path, &now, 64, 1, 1, 100ms, 1000ms);
            REQUIRE(store.open().succeeded());
            const auto admitted = store.admit(binding());
            REQUIRE(admitted.receipt.has_value());
            receipt_id = admitted.receipt->receipt_id;
        }

        now += 10000ms;
        auto reopened = store_at(temporary.path, &now, 64, 1, 1, 100ms, 1000ms);
        REQUIRE(reopened.open().succeeded());
        REQUIRE(reopened.receipt(receipt_id).has_value());
        CHECK(reopened.receipt(receipt_id)->state == ControlReceiptState::Cancelled);
        CHECK(reopened.admit(binding()).status == ControlOperationStoreStatus::Replay);
        auto next = binding("{}", "next-after-restart");
        next.request_id = "request-wire-after-restart";
        CHECK(reopened.admit(std::move(next)).status == ControlOperationStoreStatus::Admitted);
    }
}

TEST_CASE("operation store rejects empty or overlapping replay retention bounds",
          "[inspect][control][operations][retention][config]") {
    TemporaryDirectory empty_window;
    CHECK(store_at(empty_window.path, nullptr, 64, 64, 64, 0ms, 100ms).open().status ==
          ControlOperationStoreStatus::StoreUnavailable);

    TemporaryDirectory no_tombstone_interval;
    CHECK(store_at(no_tombstone_interval.path, nullptr, 64, 64, 64, 100ms, 100ms).open().status ==
          ControlOperationStoreStatus::StoreUnavailable);
}

TEST_CASE("reopen recovers running operations without replaying admission",
          "[inspect][control][operations][restart]") {
    TemporaryDirectory temporary;
    auto now = std::chrono::system_clock::time_point{1000ms};
    ControlReceiptId receipt_id;
    {
        auto store = store_at(temporary.path, &now, 64, 1, 1);
        REQUIRE(store.open().succeeded());
        auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt.has_value());
        receipt_id = admitted.receipt->receipt_id;
        REQUIRE(store.begin(receipt_id).succeeded());
    }

    auto reopened = store_at(temporary.path, &now, 64, 1, 1);
    REQUIRE(reopened.open().status == ControlOperationStoreStatus::Opened);
    const auto recovered = reopened.receipt(receipt_id);
    REQUIRE(recovered.has_value());
    CHECK(recovered->state == ControlReceiptState::UnknownNeedsRefresh);
    REQUIRE(recovered->result.result_code.has_value());
    CHECK(*recovered->result.result_code == ControlResultCode::UnknownNeedsRefresh);
    CHECK(recovered->result.retry == ControlRetryClassification::AfterRefresh);

    const auto replay = reopened.admit(binding());
    REQUIRE(replay.status == ControlOperationStoreStatus::Replay);
    REQUIRE(replay.receipt.has_value());
    CHECK(replay.receipt->receipt_id == receipt_id);
    CHECK(replay.receipt->state == ControlReceiptState::UnknownNeedsRefresh);

    auto durable = store_at(temporary.path, &now, 64, 1, 1);
    REQUIRE(durable.open().succeeded());
    CHECK(durable.receipt(receipt_id)->state == ControlReceiptState::UnknownNeedsRefresh);

    auto next = binding("{}", "next-after-running-recovery");
    next.request_id = "request-wire-after-running-recovery";
    CHECK(durable.admit(std::move(next)).status == ControlOperationStoreStatus::Admitted);
}

TEST_CASE("cancellation requests are durable first-wins and recover terminal",
          "[inspect][control][operations][cancellation]") {
    TemporaryDirectory temporary;
    auto now = std::chrono::system_clock::time_point{123456ms};
    ControlReceiptId receipt_id;
    {
        auto store = store_at(temporary.path, &now);
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt.has_value());
        receipt_id = admitted.receipt->receipt_id;

        now += 10ms;
        const auto requested = store.request_cancellation(receipt_id, "user stopped the operation");
        REQUIRE(requested.status == ControlOperationStoreStatus::CancellationRequested);
        REQUIRE(requested.receipt.has_value());
        CHECK(requested.receipt->state == ControlReceiptState::Admitted);
        CHECK(requested.receipt->cancellation_requested);
        CHECK(requested.receipt->cancellation_reason == "user stopped the operation");
        CHECK(requested.receipt->updated_at_unix_ms == 123466);

        now += 10ms;
        const auto repeated =
            store.request_cancellation(receipt_id, "a later reason must not replace the first");
        REQUIRE(repeated.status == ControlOperationStoreStatus::CancellationRequested);
        REQUIRE(repeated.receipt.has_value());
        CHECK(repeated.receipt->cancellation_reason == "user stopped the operation");
        CHECK(repeated.receipt->updated_at_unix_ms == 123466);
    }

    auto reopened = store_at(temporary.path, &now);
    REQUIRE(reopened.open().succeeded());
    const auto durable = reopened.receipt(receipt_id);
    REQUIRE(durable.has_value());
    CHECK(durable->state == ControlReceiptState::Cancelled);
    CHECK(durable->cancellation_requested);
    CHECK(durable->cancellation_reason == "user stopped the operation");
    CHECK(durable->result.result_code == ControlResultCode::Cancelled);
    CHECK(durable->result.cancellation_reason == "user stopped the operation");
    const auto refused = reopened.request_cancellation(receipt_id, "again");
    CHECK(refused.status == ControlOperationStoreStatus::InvalidTransition);
    REQUIRE(refused.receipt.has_value());
    CHECK(refused.receipt->state == ControlReceiptState::Cancelled);

    CHECK(reopened.request_cancellation(receipt_id, "").status ==
          ControlOperationStoreStatus::InvalidRequest);
    CHECK(reopened.request_cancellation(receipt_id, std::string(4097, 'x')).status ==
          ControlOperationStoreStatus::InvalidRequest);
}

TEST_CASE("receipt bindings and cancellation reject malformed committed metadata",
          "[inspect][control][operations][cancellation][security]") {
    const auto corrupt_receipt = [](const std::filesystem::path& directory,
                                    std::string_view original, std::string replacement) {
        auto store = store_at(directory);
        REQUIRE(store.open().succeeded());
        const auto admitted = store.admit(binding());
        REQUIRE(admitted.receipt.has_value());
        const auto path = directory / (admitted.receipt->receipt_id.value + ".json");
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        std::string contents((std::istreambuf_iterator<char>(input)), {});
        const auto position = contents.find(original);
        REQUIRE(position != std::string::npos);
        contents.replace(position, original.size(), std::move(replacement));
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << contents;
        REQUIRE(output.good());
    };

    SECTION("cancellation_requested must be boolean") {
        TemporaryDirectory temporary;
        corrupt_receipt(temporary.path, R"("cancellation_requested": false)",
                        R"("cancellation_requested": "false")");
        auto malformed = store_at(temporary.path);
        CHECK(malformed.open().status == ControlOperationStoreStatus::MalformedStore);
    }

    SECTION("cancellation reason must be bounded and agree with the flag") {
        TemporaryDirectory temporary;
        corrupt_receipt(temporary.path, R"("cancellation_reason": "")",
                        "\"cancellation_reason\": \"" + std::string(4097, 'x') + "\"");
        auto malformed = store_at(temporary.path);
        CHECK(malformed.open().status == ControlOperationStoreStatus::MalformedStore);
    }

    SECTION("cancellation reason must agree with the request flag") {
        TemporaryDirectory temporary;
        corrupt_receipt(temporary.path, R"("cancellation_requested": false)",
                        R"("cancellation_requested": true)");
        auto malformed = store_at(temporary.path);
        CHECK(malformed.open().status == ControlOperationStoreStatus::MalformedStore);
    }

    SECTION("every serialized binding field is covered by the binding hash") {
        TemporaryDirectory temporary;
        corrupt_receipt(temporary.path, R"("request_id": "request-wire-request-1")",
                        R"("request_id": "request-wire-request-2")");
        auto malformed = store_at(temporary.path);
        CHECK(malformed.open().status == ControlOperationStoreStatus::MalformedStore);
    }
}

TEST_CASE("malformed committed metadata fails closed while temp files are ignored",
          "[inspect][control][operations][persistence][security]") {
    TemporaryDirectory temporary;
    {
        auto initializer = store_at(temporary.path);
        REQUIRE(initializer.open().status == ControlOperationStoreStatus::Opened);
    }
    {
        std::ofstream temporary_file(temporary.path / "receipt-dead.tmp-interrupted",
                                     std::ios::binary);
        temporary_file << "{truncated";
    }
    auto clean = store_at(temporary.path);
    REQUIRE(clean.open().status == ControlOperationStoreStatus::Opened);

    const auto malformed_path = temporary.path / "receipt-00000000000000000000000000000000.json";
    {
        std::ofstream malformed(malformed_path, std::ios::binary);
        malformed << "{\"schema\":";
    }
#ifndef _WIN32
    REQUIRE(::chmod(malformed_path.c_str(), 0600) == 0);
#endif

    auto malformed = store_at(temporary.path);
    CHECK(malformed.open().status == ControlOperationStoreStatus::MalformedStore);
    CHECK_FALSE(malformed.is_open());
    CHECK(malformed.admit(binding()).status == ControlOperationStoreStatus::StoreUnavailable);
}

TEST_CASE("persisted receipt JSON is preflighted before parsing",
          "[inspect][control][operations][persistence][security]") {
    const auto write_committed = [](const std::filesystem::path& directory, std::string contents) {
        {
            auto initializer = store_at(directory);
            REQUIRE(initializer.open().succeeded());
        }
        const auto path = directory / "receipt-00000000000000000000000000000000.json";
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        REQUIRE(output.good());
#ifndef _WIN32
        REQUIRE(::chmod(path.c_str(), 0600) == 0);
#endif
    };

    SECTION("an unterminated quoted value fails closed") {
        TemporaryDirectory temporary;
        write_committed(temporary.path, R"({"schema":"dev.pulp.control/operation-receipt@1)");
        auto malformed = store_at(temporary.path);
        CHECK(malformed.open().status == ControlOperationStoreStatus::MalformedStore);
        CHECK_FALSE(malformed.is_open());
    }

    SECTION("invalid UTF-8 inside a quoted value fails closed") {
        TemporaryDirectory temporary;
        std::string invalid = R"({"schema":")";
        invalid.push_back(static_cast<char>(0xff));
        invalid += R"("})";
        write_committed(temporary.path, std::move(invalid));
        auto malformed = store_at(temporary.path);
        CHECK(malformed.open().status == ControlOperationStoreStatus::MalformedStore);
        CHECK_FALSE(malformed.is_open());
    }

    SECTION("a valid receipt exactly at the configured byte cap reopens") {
        TemporaryDirectory temporary;
        auto now = std::chrono::system_clock::time_point{123456ms};
        ControlReceiptId receipt_id;
        std::uintmax_t receipt_bytes = 0;
        {
            auto store = store_at(temporary.path, &now);
            REQUIRE(store.open().succeeded());
            const auto admitted = store.admit(binding());
            REQUIRE(admitted.receipt);
            receipt_id = admitted.receipt->receipt_id;
            REQUIRE(store.begin(receipt_id).succeeded());
            REQUIRE(store
                        .transition(receipt_id, ControlReceiptState::Running,
                                    ControlReceiptState::Completed)
                        .succeeded());
            receipt_bytes =
                std::filesystem::file_size(temporary.path / (receipt_id.value + ".json"));
        }

        auto reopened =
            store_at(temporary.path, &now, 64, 64, 64, std::chrono::hours{24},
                     std::chrono::hours{24 * 7}, static_cast<std::size_t>(receipt_bytes));
        REQUIRE(reopened.open().succeeded());
        REQUIRE(reopened.receipt(receipt_id));
        CHECK(reopened.receipt(receipt_id)->state == ControlReceiptState::Completed);
        CHECK(std::filesystem::file_size(temporary.path / (receipt_id.value + ".json")) ==
              receipt_bytes);
    }
}

TEST_CASE("receipt byte limits reject oversized commits without poisoning the ledger",
          "[inspect][control][operations][persistence][limits]") {
    auto now = std::chrono::system_clock::time_point{123456ms};

    SECTION("an admission exactly at the serialized byte limit succeeds") {
        TemporaryDirectory calibration;
        std::uintmax_t serialized_size = 0;
        {
            auto store = store_at(calibration.path, &now);
            REQUIRE(store.open().succeeded());
            const auto admitted = store.admit(binding());
            REQUIRE(admitted.receipt);
            serialized_size = std::filesystem::file_size(
                calibration.path / (admitted.receipt->receipt_id.value + ".json"));
        }

        TemporaryDirectory exact;
        auto exact_store =
            store_at(exact.path, &now, 64, 64, 64, std::chrono::hours{24},
                     std::chrono::hours{24 * 7}, static_cast<std::size_t>(serialized_size));
        REQUIRE(exact_store.open().succeeded());
        CHECK(exact_store.admit(binding()).status == ControlOperationStoreStatus::Admitted);

        TemporaryDirectory undersized;
        {
            auto small_store =
                store_at(undersized.path, &now, 64, 64, 64, std::chrono::hours{24},
                         std::chrono::hours{24 * 7}, static_cast<std::size_t>(serialized_size - 1));
            REQUIRE(small_store.open().succeeded());
            CHECK(small_store.admit(binding()).status ==
                  ControlOperationStoreStatus::PersistenceError);
            CHECK(small_store.receipts().empty());
        }
        auto healthy =
            store_at(undersized.path, &now, 64, 64, 64, std::chrono::hours{24},
                     std::chrono::hours{24 * 7}, static_cast<std::size_t>(serialized_size - 1));
        CHECK(healthy.open().succeeded());
        CHECK(healthy.receipts().empty());
    }

    SECTION("composite terminal results leave the prior receipt recoverable") {
        TemporaryDirectory calibration;
        std::uintmax_t running_size = 0;
        {
            auto store = store_at(calibration.path, &now);
            REQUIRE(store.open().succeeded());
            const auto admitted = store.admit(binding());
            REQUIRE(admitted.receipt);
            REQUIRE(store.begin(admitted.receipt->receipt_id).succeeded());
            running_size = std::filesystem::file_size(
                calibration.path / (admitted.receipt->receipt_id.value + ".json"));
        }

        TemporaryDirectory limited;
        const auto byte_limit = static_cast<std::size_t>(running_size + 256);
        ControlReceiptId receipt_id;
        {
            auto store = store_at(limited.path, &now, 64, 64, 64, std::chrono::hours{24},
                                  std::chrono::hours{24 * 7}, byte_limit);
            REQUIRE(store.open().succeeded());
            const auto admitted = store.admit(binding());
            REQUIRE(admitted.receipt);
            receipt_id = admitted.receipt->receipt_id;
            REQUIRE(store.begin(receipt_id).succeeded());

            ControlOperationResult composite;
            composite.detail_json = "{\"payload\":\"" + std::string(180, 'd') + "\"}";
            composite.artifacts.push_back({std::string(60, 'a'), std::string(60, 'm'), 42});
            composite.evidence_ids.push_back(std::string(100, 'e'));
            const auto rejected =
                store.transition(receipt_id, ControlReceiptState::Running,
                                 ControlReceiptState::Completed, std::move(composite));
            REQUIRE(rejected.status == ControlOperationStoreStatus::PersistenceError);
            REQUIRE(rejected.receipt);
            CHECK(rejected.receipt->state == ControlReceiptState::Running);
            CHECK(store.receipt(receipt_id)->state == ControlReceiptState::Running);
            CHECK(std::filesystem::file_size(limited.path / (receipt_id.value + ".json")) <=
                  byte_limit);
        }

        auto reopened = store_at(limited.path, &now, 64, 64, 64, std::chrono::hours{24},
                                 std::chrono::hours{24 * 7}, byte_limit);
        REQUIRE(reopened.open().succeeded());
        REQUIRE(reopened.receipt(receipt_id));
        CHECK(reopened.receipt(receipt_id)->state == ControlReceiptState::UnknownNeedsRefresh);
    }
}

TEST_CASE("receipt files and directories are owner-private",
          "[inspect][control][operations][persistence][security]") {
    TemporaryDirectory temporary;
    auto store = store_at(temporary.path);
    REQUIRE(store.open().succeeded());
    const auto admitted = store.admit(binding());
    REQUIRE(admitted.receipt.has_value());

#ifndef _WIN32
    struct stat directory_status{};
    struct stat receipt_status{};
    REQUIRE(::stat(temporary.path.c_str(), &directory_status) == 0);
    const auto receipt_path = temporary.path / (admitted.receipt->receipt_id.value + ".json");
    REQUIRE(::stat(receipt_path.c_str(), &receipt_status) == 0);
    CHECK((directory_status.st_mode & 077) == 0);
    CHECK((receipt_status.st_mode & 077) == 0);
#else
    const auto receipt_path = temporary.path / (admitted.receipt->receipt_id.value + ".json");
    CHECK(owner_private_windows_path(temporary.path));
    CHECK(owner_private_windows_path(receipt_path));

    SECTION("an added world-readable file ACE is rejected on reopen") {
        REQUIRE(grant_world_read(receipt_path));
        auto reopened = store_at(temporary.path);
        CHECK(reopened.open().status == ControlOperationStoreStatus::MalformedStore);
    }

    SECTION("an added world-readable directory ACE is rejected on reopen") {
        REQUIRE(grant_world_read(temporary.path));
        auto reopened = store_at(temporary.path);
        CHECK(reopened.open().status == ControlOperationStoreStatus::StoreUnavailable);
    }
#endif
}
