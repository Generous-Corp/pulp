#pragma once

// VST3 host-side state serialization + separated-controller sync.
//
// VST3 uses a separated model: an IComponent (the processor) and an
// IEditController each own their own state. A host that only saved/restored
// the component state (Pulp's original behavior) desynced the edit controller
// after a load — the vendor UI and the controller's parameter cache did not
// reflect the restored processor state.
//
// This header is the testable seam for that logic. Vst3Slot lives in an
// anonymous namespace in plugin_slot_vst3.cpp and is not reachable from a
// test, so the serialize/restore/sync logic is factored into these free
// functions that take the raw SDK interfaces. plugin_slot_vst3.cpp calls them;
// test_vst3_state_sync.cpp drives them with fake IComponent/IEditController.
//
// Container format (little-endian, self-describing):
//   magic "PV3S" (4) | version u32 | component_len u64 | component bytes
//                                  | controller_len u64 | controller bytes
// A blob without the magic is treated as a legacy raw-component-state blob
// (what Pulp wrote before this seam), so existing saved sessions still load.
//
// This header pulls in VST3 SDK interfaces, so it must only be included from
// translation units compiled with the SDK on the include path (PULP_HAS_VST3).

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace pulp::host::detail {

// Minimal IBStream backed by std::vector<uint8_t> for state round-trips.
class VectorStream final : public Steinberg::IBStream {
public:
    VectorStream() = default;
    explicit VectorStream(const std::vector<uint8_t>& src) : buf_(src) {}
    std::vector<uint8_t> take() { return std::move(buf_); }

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 num_bytes,
                                       Steinberg::int32* num_bytes_read) override {
        if (num_bytes < 0) return Steinberg::kInvalidArgument;
        Steinberg::int64 remaining = (Steinberg::int64)buf_.size() - pos_;
        Steinberg::int64 n = num_bytes < remaining ? num_bytes : remaining;
        if (n > 0) std::memcpy(buffer, buf_.data() + pos_, (size_t)n);
        pos_ += n;
        if (num_bytes_read) *num_bytes_read = (Steinberg::int32)n;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 num_bytes,
                                        Steinberg::int32* num_bytes_written) override {
        if (num_bytes < 0) return Steinberg::kInvalidArgument;
        const auto* p = static_cast<const uint8_t*>(buffer);
        buf_.insert(buf_.end(), p, p + num_bytes);
        pos_ = (Steinberg::int64)buf_.size();
        if (num_bytes_written) *num_bytes_written = num_bytes;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode,
                                       Steinberg::int64* result) override {
        Steinberg::int64 new_pos = pos_;
        switch (mode) {
            case kIBSeekSet: new_pos = pos; break;
            case kIBSeekCur: new_pos = pos_ + pos; break;
            case kIBSeekEnd: new_pos = (Steinberg::int64)buf_.size() + pos; break;
            default: return Steinberg::kInvalidArgument;
        }
        if (new_pos < 0 || new_pos > (Steinberg::int64)buf_.size())
            return Steinberg::kInvalidArgument;
        pos_ = new_pos;
        if (result) *result = pos_;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (!pos) return Steinberg::kInvalidArgument;
        *pos = pos_;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IBStream::iid)
            || Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::IBStream*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    std::vector<uint8_t> buf_;
    Steinberg::int64 pos_ = 0;
};

namespace vst3_state_detail {

constexpr unsigned char kMagic[4] = {'P', 'V', '3', 'S'};
constexpr uint32_t kVersion = 1;

inline void put_u32_le(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF));
}
inline void put_u64_le(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF));
}
inline uint32_t get_u32_le(const uint8_t* p) {
    uint32_t x = 0;
    for (int i = 0; i < 4; ++i) x |= (uint32_t)p[i] << (8 * i);
    return x;
}
inline uint64_t get_u64_le(const uint8_t* p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= (uint64_t)p[i] << (8 * i);
    return x;
}

}  // namespace vst3_state_detail

// Serialize the component's state, plus a separated controller's own state,
// into a versioned container. `separated` must be false for a combined plugin
// (controller == component) so its state is not written twice. Returns an
// empty vector if the component has no state (getState failed).
inline std::vector<uint8_t> vst3_serialize_state(Steinberg::Vst::IComponent* component,
                                                 Steinberg::Vst::IEditController* controller,
                                                 bool separated) {
    using namespace vst3_state_detail;
    if (!component) return {};

    VectorStream comp_stream;
    if (component->getState(&comp_stream) != Steinberg::kResultOk) return {};
    std::vector<uint8_t> comp = comp_stream.take();

    std::vector<uint8_t> ctrl;
    if (separated && controller) {
        VectorStream ctrl_stream;
        // A controller with no private state may return anything but kResultOk;
        // that is fine — we simply store an empty controller section.
        if (controller->getState(&ctrl_stream) == Steinberg::kResultOk) {
            ctrl = ctrl_stream.take();
        }
    }

    std::vector<uint8_t> out;
    out.reserve(4 + 4 + 8 + comp.size() + 8 + ctrl.size());
    out.insert(out.end(), kMagic, kMagic + 4);
    put_u32_le(out, kVersion);
    put_u64_le(out, comp.size());
    out.insert(out.end(), comp.begin(), comp.end());
    put_u64_le(out, ctrl.size());
    out.insert(out.end(), ctrl.begin(), ctrl.end());
    return out;
}

// Restore state produced by vst3_serialize_state (or a legacy raw-component
// blob), syncing a separated controller. For a separated controller the
// component state is pushed via setComponentState so the controller reflects
// the loaded processor state, then the controller's own state is restored.
// Returns false only if the component itself rejects the state.
inline bool vst3_restore_state(const std::vector<uint8_t>& data,
                               Steinberg::Vst::IComponent* component,
                               Steinberg::Vst::IEditController* controller,
                               bool separated) {
    using namespace vst3_state_detail;
    if (!component || data.empty()) return false;

    std::vector<uint8_t> comp;
    std::vector<uint8_t> ctrl;

    const bool has_magic = data.size() >= 4 &&
                           std::memcmp(data.data(), kMagic, 4) == 0;
    if (has_magic) {
        // All bounds checks are written as `len > remaining` with
        // remaining = data.size() - off, computed only while off <= size, so
        // they never underflow on an attacker-controlled length.
        size_t off = 4;
        if (data.size() < off + 4 + 8) return false;  // version + component_len
        (void)get_u32_le(&data[off]);
        off += 4;  // version — only v1 exists
        uint64_t comp_len = get_u64_le(&data[off]);
        off += 8;
        if (comp_len > (uint64_t)(data.size() - off)) return false;  // truncated body
        comp.assign(data.begin() + off, data.begin() + off + (size_t)comp_len);
        off += (size_t)comp_len;
        if (data.size() - off < 8) return false;  // missing controller_len
        uint64_t ctrl_len = get_u64_le(&data[off]);
        off += 8;
        if (ctrl_len > (uint64_t)(data.size() - off)) return false;  // truncated tail
        ctrl.assign(data.begin() + off, data.begin() + off + (size_t)ctrl_len);
    } else {
        // Legacy blob: the whole thing is raw component state.
        comp = data;
    }

    VectorStream comp_stream(comp);
    if (component->setState(&comp_stream) != Steinberg::kResultOk) return false;

    if (separated && controller) {
        // Push the processor state into the controller so its parameter cache
        // and UI reflect the load. Best-effort: a controller that does not
        // implement setComponentState just ignores it.
        VectorStream comp_for_ctrl(comp);
        controller->setComponentState(&comp_for_ctrl);
        if (!ctrl.empty()) {
            VectorStream ctrl_stream(ctrl);
            controller->setState(&ctrl_stream);
        }
    }
    return true;
}

}  // namespace pulp::host::detail
