#include "support/same_device_storage_probe.hpp"
void lifecycle() {
    auto owner = create();
    const auto generation = owner->device_owner_generation();
    require(generation != 0, "anonymous owner");
    for (auto kind : {Kind::ImportedHostPointer, Kind::Staged, Kind::ImportedHostPointer}) {
        const auto before = owner->stats();
        SharedIoProgramSession session;
        prepare(session, owner, kind);
        auto* provider = static_cast<Provider*>(session.owned_provider());
        require(!provider->reconfigure_storage_kind(kind), "live slots permitted reconfiguration");
        double error = 0;
        for (unsigned seq = 0; seq < 8; ++seq) {
            auto lease = session.acquire_input(seq, 0);
            require(bool(lease), "no input slot");
            fill(lease->bytes, seq);
            require(session.submit({lease->token, 0}), "submission rejected");
            auto result = await(session);
            require(bool(result), "completion timeout");
            require(result->status == SharedIoTerminalStatus::RetiredSuccess,
                    "non-success terminal");
            auto output = session.acquire_output(*result);
            require(bool(output), "output missing");
            error = std::max(error, check(output->bytes, seq));
            require(session.release_output({output->token}), "release output failed");
        }
        owner = return_owner(session);
        require(bool(owner), "physical release failed");
        require(owner->device_owner_generation() == generation, "device replaced");
        const auto after = owner->stats();
        const auto expected = kind == Kind::Staged ? 8u : 0u;
        require(after.runtime_write_buffer_calls - before.runtime_write_buffer_calls == expected &&
                    after.runtime_copy_buffer_calls - before.runtime_copy_buffer_calls ==
                        expected &&
                    after.runtime_map_async_calls - before.runtime_map_async_calls == expected,
                "transfer accounting wrong");
        require(after.allocations == after.host_frees &&
                    after.slots_created == after.slots_destroyed,
                "unbalanced lifetime");
        std::cout << "mode=" << (kind == Kind::Staged ? "staged" : "shared")
                  << " owner=" << generation << " blocks=8 max_error=" << error
                  << " transfers_each=" << expected << '\n';
    }
}
void cancelled_readback(Provider::Fault fault) {
    auto owner = create(fault);
    SharedIoProgramSession session;
    prepare(session, owner, Kind::Staged);
    auto lease = session.acquire_input(0, 0);
    require(bool(lease), "no input");
    fill(lease->bytes, 0);
    require(session.submit({lease->token, 0}), "cancel test rejected early");
    // Failed completions remain quarantined until the explicit physical barrier.
    for (unsigned i = 0; i < 100; ++i)
        session.service_until(now_ns(), now_ns() + 100000);
    require(!session.pop_completion(), "failed mapping retired without physical drain");
    if (fault == Provider::Fault::RetireAfterReadbackSuccess)
        require(static_cast<Provider*>(session.owned_provider())->stats().fault_injections == 1,
                "map-success-before-retirement ordering was not exercised");
    require(session.owned_provider()->drain(), "mapping physical drain failed");
    auto result = await(session);
    require(bool(result), "cancel callback not drained");
    require(result->status == SharedIoTerminalStatus::RetiredFailed,
            "cancelled mapping published success");
    require(!session.acquire_output(*result), "cancelled output readable");
    require(session.discard_completion(*result), "failed terminal not discarded");
    owner = return_owner(session);
    require(bool(owner), "cancelled owner not returned after drain");
    require(!owner->reconfigure_storage_kind(Kind::ImportedHostPointer), "failed device reused");
}
void failed_drain() {
    auto owner = create(Provider::Fault::FirstDrainFailure);
    SharedIoProgramSession session;
    prepare(session, owner, Kind::ImportedHostPointer);
    auto* identity = session.owned_provider();
    require(!return_owner(session), "failed drain released owner");
    require(session.owned_provider() == identity, "failed owner not retained");
    owner = return_owner(session);
    require(bool(owner), "retry failed to return owner");
    require(!owner->reconfigure_storage_kind(Kind::Staged), "failed drain device reused");
}
int main() {
    try {
        lifecycle();
        cancelled_readback(Provider::Fault::CancelReadbackMapping);
        cancelled_readback(Provider::Fault::RetireAfterReadbackSuccess);
        failed_drain();
        std::cout << "same_device_lifecycle=passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
