#include <pulp/format/host_quirks.hpp>

#include <pulp/format/host_quirks/ableton_live.hpp>
#include <pulp/format/host_quirks/ardour.hpp>
#include <pulp/format/host_quirks/auv3_cross_host.hpp>
#include <pulp/format/host_quirks/bitwig.hpp>
#include <pulp/format/host_quirks/cubase.hpp>
#include <pulp/format/host_quirks/digital_performer.hpp>
#include <pulp/format/host_quirks/fl_studio.hpp>
#include <pulp/format/host_quirks/logic_pro.hpp>
#include <pulp/format/host_quirks/pro_tools.hpp>
#include <pulp/format/host_quirks/reaper.hpp>
#include <pulp/format/host_quirks/studio_one.hpp>
#include <pulp/format/host_quirks/wavelab.hpp>
#include <pulp/format/host_version.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace pulp::format {

// ── Single source of truth for the HostQuirks field list ──────────────
//
// Every data member of `HostQuirks` is listed here exactly once. The
// list drives three generated passes (apply_filter, per-quirk override
// application, and field enumeration for `pulp doctor`) so they can
// never drift from one another. When you add a field to `HostQuirks`
// (and its tier to `HostQuirksMeta` + a row to host-quirks.json), add it
// here too — the static_assert below and the catalog parity test guard
// against forgetting. Keep declaration order in sync with the struct.
#define PULP_HOST_QUIRK_FIELDS(X)                       \
    X(synthesize_bypass_parameter)                      \
    X(clamp_latency_to_nonneg)                          \
    X(silence_unsupported_bus_arrangements)             \
    X(cubase10_async_view_resize_queue)                 \
    X(cubase10_param_gesture_ordering)                  \
    X(cubase10_fractional_scale_correction)             \
    X(cubase9_state_blob_size_validation)               \
    X(live_vst3_canresize_ignore)                       \
    X(live_vst3_windows_dpi_defer)                      \
    X(double_string_buffer_for_live_10_1_13)            \
    X(bitwig_vst3_linux_repaint_after_resize)           \
    X(bitwig_vst3_setbusarrangements_while_active)      \
    X(skip_bus_arrangement_call)                        \
    X(wavelab_vst3_defer_activation)                    \
    X(wavelab_state_blob_fallback)                      \
    X(tolerate_state_read_nontrue_status)               \
    X(fl_studio_setactive_process_mutex)                \
    X(fl_studio_state_reader_skip)                      \
    X(reaper_vst3_gesture_ordering)                     \
    X(reaper_process_while_bypassed)                    \
    X(reaper_keyboard_passthrough)                      \
    X(reaper_permissive_bus_arrangements)               \
    X(reaper_anticipative_fx_buffer_variability)        \
    X(reaper_midsession_setstate)                       \
    X(reaper_keyboard_only_space)                       \
    X(pro_tools_aax_sidechain_negotiation)              \
    X(pro_tools_aax_latency_callback_push)              \
    X(pro_tools_aax_mono_second_bus)                    \
    X(aax_vendor_version_unknown)                       \
    X(logic_au_channel_probe_cap)                       \
    X(logic_au_tail_time_conversion)                    \
    X(logic_au_v2_container_resize)                     \
    X(au_v3_bypass_dual_tracking)                        \
    X(au_v3_host_id_from_wrapper)                        \
    X(reaper_auv3_in_process_preferred_size_sync)       \
    X(studio_one_restart_component_ui_thread)           \
    X(digital_performer_param_list_reload)              \
    X(cubase13_midi_cc_param_id_stable)

// Tripwire: the field count baked into the X-macro list above. If a new
// HostQuirks field is added without extending PULP_HOST_QUIRK_FIELDS the
// generated passes would silently skip it; bump this count in lock-step.
// NOTE: this static_assert only pins the macro's OWN count (it is
// self-counting) — it cannot by itself notice a field added to the
// `HostQuirks` struct but missed in the macro. That struct↔macro/meta
// drift axis is closed by tools/scripts/test_host_quirks_catalog_parity.py,
// which parses the `HostQuirks` struct + `HostQuirksMeta` + host-quirks.json
// and asserts all three flag sets match.
namespace {
constexpr int count_quirk_fields() noexcept {
    int n = 0;
#define PULP_QUIRK_COUNT(name) ++n;
    PULP_HOST_QUIRK_FIELDS(PULP_QUIRK_COUNT)
#undef PULP_QUIRK_COUNT
    return n;
}
}  // namespace
static_assert(count_quirk_fields() == 38,
              "PULP_HOST_QUIRK_FIELDS is out of sync with HostQuirks — "
              "add the new field to the X-macro list and bump this count.");

namespace {

// Per-host quirk-population helpers. Hosts without a dedicated header
// live here as private helpers until they are extracted. License-hygiene:
// every flag flipped here (or in a per-host header) must be backed by a
// host vendor doc + a reproducer Pulp issue. See the catalog at
// `core/format/host-quirks.json`.

void apply_reaper_quirks(HostQuirks& q, HostVersion v) {
    // Main 6 REAPER rows (15 + R1-R4 + R6).
    host_quirks::apply_reaper(q, v);
    // Layer the keyboard-only-space lesson on top so its LessonOnly
    // tier can evolve independently of the rest of the REAPER dispatch.
    host_quirks::apply_reaper_keyboard(q, v);
    // Pulp #3044: REAPER hosts AU v3 in-process and needs
    // preferredContentSize set synchronously during audioUnitInitialized.
    // Layered separately so the LessonOnly tier can evolve independently
    // of the rest of the REAPER dispatch.
    host_quirks::apply_reaper_auv3_in_process(q, v);
}

void apply_pro_tools_quirks(HostQuirks& q, HostVersion v) {
    // Main 3 AAX rows (16-18). Adapter use is gated on the
    // developer-supplied Avid SDK; the quirk dispatch itself is
    // unconditional so filtering behaves identically across builds.
    host_quirks::apply_pro_tools(q, v);
    // Pro Tools' AAX wrapper does not surface a reliable vendor version
    // through the AAX spec, so version-gated decisions for this host
    // must be skipped entirely. Layered via the separate factory so its
    // LessonOnly tier can evolve independently of rows 16-18.
    host_quirks::apply_pro_tools_aax_vendor_version_unknown(q, v);
}

// Tier filtering. A single helper reduces a (status-tag, current value)
// pair to either the current value or the field's "off" value,
// depending on `filter`. Bool fields go to `false` when filtered out
// (so `kQuirkFilterOff` zeroes everything including cheap defenses).
// Numeric fields fall back to their default-constructed value (so the
// Logic channel-probe cap reverts to the cross-host 64, not to 0).
constexpr bool keep(QuirkStatus status, QuirkFilter filter) noexcept {
    switch (status) {
        case QuirkStatus::Validated:   return filter.allow_validated;
        case QuirkStatus::Speculative: return filter.allow_speculative;
        case QuirkStatus::LessonOnly:  return filter.allow_lesson_only;
    }
    return false;
}

constexpr void reset_if_filtered(bool& field, bool /*default_value*/,
                                 QuirkStatus status, QuirkFilter filter) noexcept {
    if (!keep(status, filter)) field = false;
}

constexpr void reset_if_filtered(int& field, int default_value,
                                 QuirkStatus status, QuirkFilter filter) noexcept {
    if (!keep(status, filter)) field = default_value;
}

// Compile-time default sentinel — used by reset_if_filtered to restore
// numeric fields to the value `HostQuirks{}` would yield (e.g. the
// Logic channel-probe cap to its cross-host default of 64). Keeping
// it as a constexpr instance (rather than building a fresh one per
// call) makes apply_filter() noexcept + cheap.
constexpr HostQuirks kDefaultHostQuirks{};

// Compile-time policy selection (the baseline the runtime layer falls
// through to). The CMake option `PULP_HOST_QUIRKS_DEFAULT_POLICY`
// (core/format/CMakeLists.txt) defines `_VALIDATED_ONLY` (the shipped
// default — only bench-validated accommodations fire), `_OFF`, or
// neither (`all` — every detected quirk fires; the pre-enforcement
// behavior, kept as an explicit opt-in).
constexpr QuirkFilter default_policy_filter() noexcept {
#if defined(PULP_HOST_QUIRKS_DEFAULT_POLICY_OFF)
    return kQuirkFilterOff;
#elif defined(PULP_HOST_QUIRKS_DEFAULT_POLICY_VALIDATED_ONLY)
    return kQuirkFilterValidatedOnly;
#else
    return QuirkFilter{}; // all tiers allowed
#endif
}

// ── Runtime policy layer ──────────────────────────────────────────────
//
// Layered on TOP of the compile-time default above. Precedence (highest
// first): per-quirk override > set_host_quirk_policy() (API) >
// PULP_HOST_QUIRKS env > compile-time default. All of this is init-time
// state (adapters resolve once at construction); it is NOT touched on
// the audio thread. A single mutex guards the API-set state.

// Parse PULP_HOST_QUIRKS once. Recognized (case-insensitive):
//   off | validated-only (or validated_only) | all.
// An empty/unset var → nullopt (fall through). An unrecognized value →
// nullopt + a one-time stderr warning (so a typo can't silently disable
// accommodations).
std::optional<QuirkFilter> parse_env_policy() {
    const char* raw = std::getenv("PULP_HOST_QUIRKS");
    if (raw == nullptr || raw[0] == '\0') return std::nullopt;
    std::string v(raw);
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "off") return kQuirkFilterOff;
    if (v == "all") return QuirkFilter{};
    if (v == "validated-only" || v == "validated_only" || v == "validatedonly")
        return kQuirkFilterValidatedOnly;
    std::fprintf(stderr,
                 "[pulp] warning: ignoring unrecognized PULP_HOST_QUIRKS=%s "
                 "(expected off|validated-only|all)\n",
                 raw);
    return std::nullopt;
}

const std::optional<QuirkFilter>& env_policy() {
    static const std::optional<QuirkFilter> cached = parse_env_policy();
    return cached;
}

std::mutex& policy_mutex() {
    static std::mutex m;
    return m;
}

// API-set whole-policy filter (highest base-layer precedence). nullopt
// means "not set — fall through to env/compile".
std::optional<QuirkFilter>& api_policy() {
    static std::optional<QuirkFilter> p;
    return p;
}

// Per-quirk forced state by field name. true → force the host-populated
// value (exempt from the tier filter); false → force the default (off).
std::map<std::string, bool, std::less<>>& quirk_overrides() {
    static std::map<std::string, bool, std::less<>> m;
    return m;
}

} // namespace

void apply_filter(HostQuirks& q, QuirkFilter filter) {
    constexpr auto& m = kHostQuirksMeta;
    constexpr auto& d = kDefaultHostQuirks;

    // Reset every field whose meta tier is not allowed by `filter`. The
    // field list lives in the single PULP_HOST_QUIRK_FIELDS X-macro so
    // this pass, the per-quirk override pass, and the doctor enumeration
    // all iterate identical sets (no manual list to drift).
#define PULP_QUIRK_FILTER_FIELD(name) reset_if_filtered(q.name, d.name, m.name, filter);
    PULP_HOST_QUIRK_FIELDS(PULP_QUIRK_FILTER_FIELD)
#undef PULP_QUIRK_FILTER_FIELD
}

HostQuirks make_quirks_for(HostType type, HostVersion version) {
    HostQuirks q; // cheap defenses on by default
    switch (type) {
        case HostType::Cubase:
        case HostType::Nuendo:        host_quirks::apply_cubase(q, version); break;
        case HostType::AbletonLive:   host_quirks::apply_ableton_live(q, version); break;
        case HostType::Wavelab:       host_quirks::apply_wavelab(q, version); break;
        case HostType::Bitwig:        host_quirks::apply_bitwig(q, version); break;
        case HostType::FLStudio:      host_quirks::apply_fl_studio(q, version); break;
        case HostType::Reaper:        apply_reaper_quirks(q, version); break;
        case HostType::LogicPro:
            // Logic + GarageBand share the AU v2 host stack (rows 19,
            // 20) and also expose an AU v3 surface (rows 21, 22). Apply
            // the per-host Logic helper first, then layer the AU v3
            // cross-host flags on top so adapters consulting either
            // surface see consistent state.
            host_quirks::apply_logic_pro(q, version);
            // The AU v2 immediate-container resize has only been validated
            // against Logic. GarageBand shares other AU hosting behavior but
            // must not inherit a mutation of its host-owned Cocoa hierarchy.
            q.logic_au_v2_container_resize = true;
            host_quirks::apply_auv3_cross_host(q, version);
            break;
        case HostType::GarageBand:
            host_quirks::apply_logic_pro(q, version);
            host_quirks::apply_auv3_cross_host(q, version);
            break;
        case HostType::ProTools:      apply_pro_tools_quirks(q, version); break;
        case HostType::Ardour:        host_quirks::apply_ardour(q, version); break;
        case HostType::Mixbus32C:     host_quirks::apply_mixbus32c(q, version); break;
        case HostType::StudioOne:     host_quirks::apply_studio_one(q, version); break;
        case HostType::DigitalPerformer:
            host_quirks::apply_digital_performer(q, version);
            break;
        default: break;
    }
    return q;
}

HostQuirks make_quirks_for_validated_only(HostType type, HostVersion version) {
    auto q = make_quirks_for(type, version);
    apply_filter(q, kQuirkFilterValidatedOnly);
    return q;
}

ResolvedQuirkPolicy resolve_quirk_policy() {
    std::lock_guard<std::mutex> lk(policy_mutex());
    if (api_policy().has_value())
        return {*api_policy(), QuirkPolicySource::Api};
    if (env_policy().has_value())
        return {*env_policy(), QuirkPolicySource::Environment};
    return {default_policy_filter(), QuirkPolicySource::CompileDefault};
}

void set_host_quirk_policy(std::optional<QuirkFilter> filter) {
    std::lock_guard<std::mutex> lk(policy_mutex());
    api_policy() = filter;
}

void set_quirk_override(std::string_view flag, bool enabled) {
    std::lock_guard<std::mutex> lk(policy_mutex());
    quirk_overrides()[std::string(flag)] = enabled;
}

void clear_quirk_overrides() {
    std::lock_guard<std::mutex> lk(policy_mutex());
    quirk_overrides().clear();
}

namespace {
// "Enforced" for the doctor view = the accommodation is actively in
// effect. The meaning differs by field type, so these overloads pick
// the right test per field (resolved at the X-macro expansion site):
//   * bool flag  → enforced when true (the defense/workaround is on).
//   * int field  → enforced when it differs from the cross-host default
//     (e.g. Logic's channel-probe cap of 8 vs the default 64); the
//     default value is "no host-specific accommodation".
constexpr bool quirk_field_enforced(bool value, bool /*default_value*/) noexcept {
    return value;
}
constexpr bool quirk_field_enforced(int value, int default_value) noexcept {
    return value != default_value;
}

// The "off" value a force-off override sets — disabling an accommodation,
// which is NOT the same as the struct default for cheap defenses (whose
// default is true). Mirrors reset_if_filtered's behavior: bool → false,
// int → its cross-host default (no host-specific cap).
constexpr bool quirk_off_value(bool /*default_value*/) noexcept { return false; }
constexpr int  quirk_off_value(int default_value) noexcept { return default_value; }

// Apply per-quirk overrides on top of an already tier-filtered `q`.
// `populated` is the unfiltered host result, so force-on restores the
// real host value (handles the int field too); force-off resets to the
// struct default. Field dispatch is generated from PULP_HOST_QUIRK_FIELDS.
void apply_overrides(HostQuirks& q, const HostQuirks& populated) {
    std::lock_guard<std::mutex> lk(policy_mutex());
    auto& ov = quirk_overrides();
    if (ov.empty()) return;
    constexpr auto& d = kDefaultHostQuirks;
#define PULP_QUIRK_OVERRIDE_FIELD(name)                       \
    if (auto it = ov.find(#name); it != ov.end())             \
        q.name = it->second ? populated.name : quirk_off_value(d.name);
    PULP_HOST_QUIRK_FIELDS(PULP_QUIRK_OVERRIDE_FIELD)
#undef PULP_QUIRK_OVERRIDE_FIELD
}
}  // namespace

std::vector<QuirkFieldStatus> enumerate_quirk_fields(const HostQuirks& q) {
    constexpr auto& m = kHostQuirksMeta;
    constexpr auto& d = kDefaultHostQuirks;
    std::vector<QuirkFieldStatus> out;
    out.reserve(count_quirk_fields());
    // `enforced` = the field differs from its default — true means the
    // accommodation is active (covers both bool flags and the int
    // logic_au_channel_probe_cap, whose default is 64).
#define PULP_QUIRK_ENUM_FIELD(name) \
    out.push_back(QuirkFieldStatus{#name, m.name, quirk_field_enforced(q.name, d.name)});
    PULP_HOST_QUIRK_FIELDS(PULP_QUIRK_ENUM_FIELD)
#undef PULP_QUIRK_ENUM_FIELD
    return out;
}

HostQuirks resolved_quirks(HostType type, HostVersion version) {
    const HostQuirks populated = make_quirks_for(type, version);
    HostQuirks q = populated;
    apply_filter(q, resolve_quirk_policy().filter);
    apply_overrides(q, populated);
    return q;
}

HostQuirks detect_quirks() {
    const auto info = detect_host_info();
    return resolved_quirks(info.type, info.version);
}

}  // namespace pulp::format
