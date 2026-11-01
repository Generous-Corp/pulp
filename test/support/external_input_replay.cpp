#include "external_input_replay.hpp"
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>

namespace pulp::test {
namespace {
using Json = choc::value::Value;
using View = choc::value::ValueView;
using Frame = runtime::ExternalInputFrame<64>;
bool number(View v, double& out) {
    if (!v.isFloat() && !v.isInt())
        return false;
    out = v.isFloat() ? v.getFloat64() : static_cast<double>(v.getInt64());
    return std::isfinite(out);
}
bool u64(View o, const char* k, std::uint64_t& out) {
    if (!o.hasObjectMember(k) || (!o[k].isInt() && !o[k].isFloat()))
        return false;
    const auto value = o[k].isInt() ? static_cast<double>(o[k].getInt64()) : o[k].getFloat64();
    if (!std::isfinite(value) || value < 0 || std::floor(value) != value)
        return false;
    out = static_cast<std::uint64_t>(value);
    return true;
}
bool u32(View o, const char* k, std::uint32_t& out) {
    std::uint64_t n = 0;
    if (!u64(o, k, n) || n > UINT32_MAX)
        return false;
    out = static_cast<std::uint32_t>(n);
    return true;
}
std::optional<runtime::ExternalInputStatus> status(std::string_view v) {
    if (v == "valid")
        return runtime::ExternalInputStatus::valid;
    if (v == "invalid")
        return runtime::ExternalInputStatus::invalid;
    if (v == "denied")
        return runtime::ExternalInputStatus::denied;
    if (v == "paused")
        return runtime::ExternalInputStatus::paused;
    return std::nullopt;
}
const char* status_name(runtime::ExternalInputStatus s) {
    switch (s) {
    case runtime::ExternalInputStatus::valid:
        return "valid";
    case runtime::ExternalInputStatus::invalid:
        return "invalid";
    case runtime::ExternalInputStatus::unavailable:
        return "unavailable";
    case runtime::ExternalInputStatus::denied:
        return "denied";
    case runtime::ExternalInputStatus::paused:
        return "paused";
    }
    return "unknown";
}
const char* verdict(runtime::ExternalInputVerdict v) {
    switch (v) {
    case runtime::ExternalInputVerdict::accepted:
        return "accepted";
    case runtime::ExternalInputVerdict::unchanged:
        return "unchanged";
    case runtime::ExternalInputVerdict::rejected_status:
        return "rejected_status";
    case runtime::ExternalInputVerdict::rejected_source:
        return "rejected_source";
    case runtime::ExternalInputVerdict::rejected_order:
        return "rejected_order";
    case runtime::ExternalInputVerdict::rejected_malformed:
        return "rejected_malformed";
    case runtime::ExternalInputVerdict::rejected_confidence:
        return "rejected_confidence";
    }
    return "unknown";
}
bool known(View o, std::initializer_list<const char*> fields) {
    for (std::uint32_t i = 0; i < o.size(); ++i) {
        const auto n = o.getObjectMemberAt(i).name;
        bool found = false;
        for (const auto* f : fields)
            found |= std::string(n) == f;
        if (!found)
            return false;
    }
    return true;
}
} // namespace
ExternalInputReplayReceipt run_external_input_replay(const std::filesystem::path& path,
                                                     runtime::ExternalInputGate<64>& gate,
                                                     runtime::ExternalInputPolicy& policy) {
    ExternalInputReplayReceipt receipt;
    receipt.fixture = path.string();
    std::ifstream input(path);
    std::string line;
    std::uint64_t block = 128, capacity = 64;
    bool header = false;
    std::optional<runtime::ExternalInputVerdict> last;
    while (std::getline(input, line)) {
        Json o;
        try {
            o = choc::json::parse(line);
        } catch (...) {
            ++receipt.failed;
            continue;
        }
        if (!o.isObject()) {
            ++receipt.failed;
            continue;
        }
        if (!header) {
            if (!o.hasObjectMember("schema") || !o.hasObjectMember("capacity") ||
                !o.hasObjectMember("block_samples") || !o["policy"].isObject() ||
                !u64(o, "block_samples", block)) {
                ++receipt.failed;
                continue;
            }
            std::uint64_t cap = 0, source = 0, age = 0, count = 0;
            double min = 0;
            const auto p = o["policy"];
            if (!u64(o, "capacity", cap) || cap == 0 || cap > 64 || !u64(p, "source_id", source) ||
                !number(p["minimum_confidence"], min) || !u64(p, "max_age_samples", age) ||
                !u64(p, "expected_count", count) || source > UINT32_MAX || count > 64) {
                ++receipt.failed;
                continue;
            }
            capacity = cap;
            policy = {static_cast<std::uint32_t>(source), static_cast<float>(min), age,
                      static_cast<std::uint8_t>(count)};
            header = true;
            continue;
        }
        const auto type = o["t"].getWithDefault<std::string>("");
        if (type == "frame") {
            if (!known(o, {"t", "source_id", "epoch", "sequence", "provider_time_ns", "status",
                           "confidence", "values"}) ||
                !o["status"].isString() || !o["values"].isArray()) {
                ++receipt.failed;
                continue;
            }
            Frame f;
            double confidence = 0;
            if (!u32(o, "source_id", f.source_id) || !u32(o, "epoch", f.epoch) ||
                !u64(o, "sequence", f.sequence) ||
                !u64(o, "provider_time_ns", f.provider_time_ns) ||
                !number(o["confidence"], confidence) || o["values"].size() > 64) {
                ++receipt.failed;
                continue;
            }
            const auto parsed_status = status(o["status"].getString());
            if (!parsed_status) {
                ++receipt.failed;
                continue;
            }
            f.status = *parsed_status;
            f.confidence = static_cast<float>(confidence);
            f.count = static_cast<std::uint8_t>(o["values"].size());
            if (f.count > capacity) {
                ++receipt.failed;
                continue;
            }
            for (std::uint32_t i = 0; i < f.count; ++i) {
                double value = 0;
                if (!number(o["values"][i], value)) {
                    f.count = 0;
                    break;
                }
                f.values[i] = static_cast<float>(value);
            }
            if (f.count == 0) {
                ++receipt.failed;
                continue;
            }
            last = gate.submit(f, policy);
            ++receipt.frames;
        } else if (type == "advance") {
            std::uint64_t blocks = 0;
            if (!known(o, {"t", "blocks"}) || !u64(o, "blocks", blocks)) {
                ++receipt.failed;
                continue;
            }
            gate.advance(blocks * block, policy);
            ++receipt.advances;
        } else if (type == "reset") {
            if (!known(o, {"t"})) {
                ++receipt.failed;
                continue;
            }
            gate.expire();
            ++receipt.resets;
        } else if (type == "expect") {
            if (!known(o, {"t", "verdict", "valid", "values", "tolerance", "epoch", "sequence",
                           "status"})) {
                ++receipt.failed;
                continue;
            }
            ++receipt.expects;
            if (o.hasObjectMember("verdict")) {
                ++receipt.checked;
                if (!last || !o["verdict"].isString() ||
                    std::string(verdict(*last)) != o["verdict"].getString())
                    ++receipt.failed;
            }
            if (o.hasObjectMember("valid")) {
                ++receipt.checked;
                if (!o["valid"].isBool() || gate.valid() != o["valid"].getBool())
                    ++receipt.failed;
            }
            if (o.hasObjectMember("status")) {
                ++receipt.checked;
                if (!o["status"].isString() ||
                    std::string(status_name(gate.last_status())) != o["status"].getString())
                    ++receipt.failed;
            }
            if (o.hasObjectMember("values")) {
                double tolerance = 0;
                if (!o["values"].isArray() ||
                    (o.hasObjectMember("tolerance") && !number(o["tolerance"], tolerance))) {
                    ++receipt.failed;
                    continue;
                }
                for (std::uint32_t i = 0; i < o["values"].size(); ++i) {
                    double expected = 0;
                    ++receipt.checked;
                    if (!number(o["values"][i], expected) || i >= gate.values().size() ||
                        !std::isfinite(gate.values()[i]) ||
                        std::abs(static_cast<double>(gate.values()[i]) - expected) > tolerance)
                        ++receipt.failed;
                }
            }
            for (const auto* key : {"epoch", "sequence"})
                if (o.hasObjectMember(key)) {
                    ++receipt.checked;
                    std::uint64_t expected = 0;
                    if (!u64(o, key, expected) ||
                        (std::string(key) == "epoch" ? gate.epoch() : gate.sequence()) != expected)
                        ++receipt.failed;
                }
        } else
            ++receipt.failed;
    }
    receipt.result = receipt.failed ? "fail" : (receipt.expects ? "pass" : "inconclusive");
    return receipt;
}
bool write_external_input_receipt(const std::filesystem::path& path,
                                  const ExternalInputReplayReceipt& r, std::string& error) {
    std::ofstream out(path);
    if (!out) {
        error = "cannot open receipt";
        return false;
    }
    out << "{\"schema\":\"pulp.external-input-receipt.v1\",\"fixture\":\"" << r.fixture
        << "\",\"records\":{\"frames\":" << r.frames << ",\"advances\":" << r.advances
        << ",\"expects\":" << r.expects << ",\"resets\":" << r.resets
        << "},\"expectations\":{\"checked\":" << r.checked << ",\"failed\":" << r.failed
        << "},\"result\":\"" << r.result << "\"}\n";
    if (!out.good()) {
        error = "cannot write receipt";
        return false;
    }
    return true;
}
} // namespace pulp::test
