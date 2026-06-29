// Throwing hot-reload logic fixture for test_reload_transaction.cpp.
//
// Passes the ABI-version and build-fingerprint gates (same build as the shell),
// then throws from its factory — so the transaction must catch it across the
// dlopen seam and return RejectedCandidateThrew rather than letting the
// exception escape. Built as a MODULE and dlopen'd.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>

#include <stdexcept>

using namespace pulp;

namespace {
format::Processor* make_throwing() {
    throw std::runtime_error("boom: logic factory failed to construct");
}
}  // namespace

PULP_RELOAD_LOGIC(make_throwing())
