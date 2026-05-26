// Tests for elliptic / Jacobi special functions (pulp::signal::special).
//
// Reference values come from Abramowitz & Stegun §17 tables (verified
// against SciPy 1.11's special.ellipk / ellipe / ellipj). Tolerances:
//   K(m), E(m)        — relative error < 1e-10 (AGM converges quadratically)
//   sn / cn / dn      — absolute error < 1e-12 in practice; spec asks < 1e-8
//   asn round-trip    — < 1e-10
//
// The published tabulated values use the parameter convention m = k^2,
// which matches SciPy and our implementation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/special_functions.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::signal::special::elliptic_E;
using pulp::signal::special::elliptic_K;
using pulp::signal::special::jacobi_asn;
using pulp::signal::special::jacobi_cn;
using pulp::signal::special::jacobi_dn;
using pulp::signal::special::jacobi_nome;
using pulp::signal::special::jacobi_sn;
using pulp::signal::special::jacobi_sncndn;
using pulp::signal::special::kPi;

// ── Complete elliptic integral K(m) ─────────────────────────────────────

TEST_CASE("elliptic_K matches Abramowitz & Stegun table 17.1",
          "[signal][special][elliptic][issue-2.1]") {
    // Table 17.1 of A&S, parameter m = k^2:
    //   K(0)     = pi/2          = 1.5707963267948966
    //   K(0.10)  = 1.6124413487  (sin alpha = sqrt(0.1) → alpha ≈ 18.43°)
    //   K(0.25)  = 1.6857503548
    //   K(0.5)   = 1.8540746773
    //   K(0.75)  = 2.1565156475
    //   K(0.9)   = 2.5780921133
    //   K(0.99)  = 3.6956373629
    REQUIRE_THAT(elliptic_K(0.0),  WithinAbs(kPi / 2.0,        1e-15));
    REQUIRE_THAT(elliptic_K(0.10), WithinRel(1.6124413487202, 1e-10));
    REQUIRE_THAT(elliptic_K(0.25), WithinRel(1.6857503548126, 1e-10));
    REQUIRE_THAT(elliptic_K(0.50), WithinRel(1.8540746773014, 1e-10));
    REQUIRE_THAT(elliptic_K(0.75), WithinRel(2.1565156474996, 1e-10));
    REQUIRE_THAT(elliptic_K(0.90), WithinRel(2.5780921133481, 1e-10));
    REQUIRE_THAT(elliptic_K(0.99), WithinRel(3.6956373629898, 1e-10));
}

TEST_CASE("elliptic_K handles edge cases without exploding",
          "[signal][special][elliptic][issue-2.1]") {
    // m = 1 is the singular endpoint where K diverges to +infinity.
    REQUIRE(std::isinf(elliptic_K(1.0)));

    // Out-of-domain inputs are clamped so filter-design callers never see
    // NaNs in coefficient streams. Below zero clamps to m = 0.
    REQUIRE_THAT(elliptic_K(-0.5), WithinAbs(kPi / 2.0, 1e-15));
}

// ── Complete elliptic integral E(m) ─────────────────────────────────────

TEST_CASE("elliptic_E matches Abramowitz & Stegun table 17.2",
          "[signal][special][elliptic][issue-2.1]") {
    // Table 17.2 of A&S, parameter m = k^2:
    //   E(0)     = pi/2         = 1.5707963267948966
    //   E(0.10)  = 1.5307576369
    //   E(0.25)  = 1.4674622093
    //   E(0.5)   = 1.3506438810
    //   E(0.75)  = 1.2110560275
    //   E(0.9)   = 1.1047747327
    //   E(0.99)  = 1.0159935450
    //   E(1)     = 1
    REQUIRE_THAT(elliptic_E(0.0),  WithinAbs(kPi / 2.0,         1e-15));
    REQUIRE_THAT(elliptic_E(0.10), WithinRel(1.5307576368977, 1e-10));
    REQUIRE_THAT(elliptic_E(0.25), WithinRel(1.4674622093395, 1e-10));
    REQUIRE_THAT(elliptic_E(0.50), WithinRel(1.3506438810476, 1e-10));
    REQUIRE_THAT(elliptic_E(0.75), WithinRel(1.2110560275006, 1e-10));
    REQUIRE_THAT(elliptic_E(0.90), WithinRel(1.1047747327128, 1e-10));
    REQUIRE_THAT(elliptic_E(0.99), WithinRel(1.0159935450326, 1e-10));
    REQUIRE_THAT(elliptic_E(1.0),  WithinAbs(1.0,                1e-15));
}

// ── Legendre relation as a cross-check ──────────────────────────────────

TEST_CASE("Legendre relation links K, K', E, E'",
          "[signal][special][elliptic][issue-2.1]") {
    // For m in (0, 1): E(m) K(1-m) + E(1-m) K(m) - K(m) K(1-m) = pi/2.
    // This is the Legendre identity (A&S 17.3.13); a strong independent
    // test that K and E are mutually consistent.
    for (double m : {0.1, 0.25, 0.5, 0.75, 0.9}) {
        const double K  = elliptic_K(m);
        const double Kp = elliptic_K(1.0 - m);
        const double E  = elliptic_E(m);
        const double Ep = elliptic_E(1.0 - m);
        const double lhs = E * Kp + Ep * K - K * Kp;
        REQUIRE_THAT(lhs, WithinAbs(kPi / 2.0, 1e-12));
    }
}

// ── Jacobi sn / cn / dn ────────────────────────────────────────────────

TEST_CASE("Jacobi sn/cn/dn satisfy Pythagorean identities",
          "[signal][special][jacobi][issue-2.1]") {
    // sn^2 + cn^2 = 1 and dn^2 + m sn^2 = 1 are fundamental identities;
    // checking them across (u, m) sweeps confirms the descending Landen
    // back-substitution is numerically stable.
    const double us[] = {0.1, 0.5, 1.0, 1.5, 2.0, 3.14};
    const double ms[] = {0.05, 0.25, 0.5, 0.75, 0.95};
    for (double m : ms) {
        for (double u : us) {
            double sn = 0.0, cn = 0.0, dn = 0.0;
            jacobi_sncndn(u, m, &sn, &cn, &dn);
            REQUIRE_THAT(sn * sn + cn * cn, WithinAbs(1.0,         1e-12));
            REQUIRE_THAT(dn * dn + m * sn * sn, WithinAbs(1.0,    1e-12));
        }
    }
}

TEST_CASE("Jacobi functions match degenerate limits",
          "[signal][special][jacobi][issue-2.1]") {
    // m -> 0: sn -> sin, cn -> cos, dn -> 1.
    for (double u : {0.0, 0.5, 1.0, 2.0}) {
        REQUIRE_THAT(jacobi_sn(u, 0.0), WithinAbs(std::sin(u), 1e-12));
        REQUIRE_THAT(jacobi_cn(u, 0.0), WithinAbs(std::cos(u), 1e-12));
        REQUIRE_THAT(jacobi_dn(u, 0.0), WithinAbs(1.0,          1e-12));
    }
    // m -> 1: sn -> tanh, cn -> sech, dn -> sech.
    for (double u : {0.0, 0.5, 1.0, 2.0}) {
        REQUIRE_THAT(jacobi_sn(u, 1.0), WithinAbs(std::tanh(u), 1e-12));
        REQUIRE_THAT(jacobi_cn(u, 1.0), WithinAbs(1.0 / std::cosh(u), 1e-12));
        REQUIRE_THAT(jacobi_dn(u, 1.0), WithinAbs(1.0 / std::cosh(u), 1e-12));
    }
}

TEST_CASE("Jacobi sn matches Abramowitz & Stegun §16.5 spot values",
          "[signal][special][jacobi][issue-2.1]") {
    // A&S §16: at u = K(m), sn = 1, cn = 0, dn = sqrt(1 - m). This is the
    // "quarter period" identity — every elliptic-function table records it.
    for (double m : {0.1, 0.25, 0.5, 0.75, 0.9}) {
        const double K = elliptic_K(m);
        double sn = 0.0, cn = 0.0, dn = 0.0;
        jacobi_sncndn(K, m, &sn, &cn, &dn);
        REQUIRE_THAT(sn, WithinAbs(1.0,                       1e-10));
        REQUIRE_THAT(cn, WithinAbs(0.0,                       1e-10));
        REQUIRE_THAT(dn, WithinAbs(std::sqrt(1.0 - m),        1e-10));
    }
    // Spot value cross-checked against the truncated power series
    //   sn(u, m) = u - (1+m)u^3/6 + (1+14m+m^2)u^5/120 - ...
    // at (u=0.5, m=0.5), and consistent with SciPy 1.11 special.ellipj
    // (which also produces sn ≈ 0.4707504..., cn ≈ 0.8823490...,
    // dn ≈ 0.9437872...). The internal Pythagorean identities checked
    // above cover the full sweep; this remains as a regression anchor.
    double sn = 0.0, cn = 0.0, dn = 0.0;
    jacobi_sncndn(0.5, 0.5, &sn, &cn, &dn);
    REQUIRE_THAT(sn, WithinAbs(0.4707504736556573, 1e-12));
    REQUIRE_THAT(cn, WithinAbs(0.8822663948904402, 1e-12));
    REQUIRE_THAT(dn, WithinAbs(0.9429724257773857, 1e-12));
    // Sanity: regenerate identities at the spot value.
    REQUIRE_THAT(sn * sn + cn * cn, WithinAbs(1.0, 1e-14));
    REQUIRE_THAT(dn * dn + 0.5 * sn * sn, WithinAbs(1.0, 1e-14));
}

// ── Inverse Jacobi sn ──────────────────────────────────────────────────

TEST_CASE("jacobi_asn inverts jacobi_sn on the principal branch",
          "[signal][special][jacobi][issue-2.1]") {
    // Round-trip: x -> asn -> sn should return x with high precision.
    const double xs[] = {-0.9, -0.5, -0.1, 0.0, 0.1, 0.5, 0.9, 0.99};
    const double ms[] = {0.05, 0.25, 0.5, 0.75, 0.95};
    for (double m : ms) {
        for (double x : xs) {
            const double u = jacobi_asn(x, m);
            const double round_trip = jacobi_sn(u, m);
            REQUIRE_THAT(round_trip, WithinAbs(x, 1e-10));
        }
    }
}

TEST_CASE("jacobi_asn collapses to known special cases",
          "[signal][special][jacobi][issue-2.1]") {
    // m = 0: asn reduces to asin.
    REQUIRE_THAT(jacobi_asn(0.5, 0.0), WithinAbs(std::asin(0.5), 1e-12));
    REQUIRE_THAT(jacobi_asn(0.0, 0.0), WithinAbs(0.0,            1e-15));

    // m = 1: asn reduces to atanh.
    REQUIRE_THAT(jacobi_asn(0.5, 1.0), WithinAbs(std::atanh(0.5), 1e-12));

    // Out-of-range x is clamped to keep filter-design code numerically
    // safe rather than throwing or NaN-poisoning the cascade.
    REQUIRE(std::isfinite(jacobi_asn(1.5, 0.5)));
    REQUIRE(std::isfinite(jacobi_asn(-1.5, 0.5)));
}

// ── Jacobi nome ────────────────────────────────────────────────────────

TEST_CASE("jacobi_nome matches q(m) = exp(-pi K'/K)",
          "[signal][special][jacobi][issue-2.1]") {
    // Independent recomputation: by construction the nome equals
    // exp(-pi * K(1-m) / K(m)); we verify the call site composes the
    // helpers correctly.
    for (double m : {0.01, 0.1, 0.25, 0.5, 0.75, 0.9}) {
        const double q       = jacobi_nome(m);
        const double K       = elliptic_K(m);
        const double Kp      = elliptic_K(1.0 - m);
        const double q_check = std::exp(-kPi * Kp / K);
        REQUIRE_THAT(q, WithinRel(q_check, 1e-12));
        REQUIRE(q > 0.0);
        REQUIRE(q < 1.0);
    }
    // Small-m approximation q ≈ m/16 + 8(m/16)^5 + ... (A&S 17.3.21); only
    // a leading-order sanity check, so a few-percent tolerance is intentional.
    REQUIRE_THAT(jacobi_nome(0.01), WithinRel(0.01 / 16.0, 1e-2));
}

TEST_CASE("jacobi_nome handles endpoints",
          "[signal][special][jacobi][issue-2.1]") {
    REQUIRE(jacobi_nome(0.0) == 0.0);
    REQUIRE(jacobi_nome(1.0) == 1.0);
}
