//! Integration tests locking the `trace-processor` tool-registry entry to the
//! verified fetcher that actually installs it.
//!
//! `pulp tool install trace-processor` is a registry-surfaced peer of `pulp
//! trace fetch`: both download the same pinned Perfetto `trace_processor_shell`
//! and verify it against the SHA-256 pins baked into `cmd::trace_fetch`. The
//! registry entry (`tools/packages/tool-registry.json`) therefore duplicates
//! the version + per-platform URLs by necessity — the tool surface reads the
//! registry, the fetcher owns the pins. These tests fail the moment the two
//! drift, so a version bump can't silently update one and leave `pulp tool
//! info` advertising a stale artifact.

use std::path::PathBuf;

use pulp_rs::cmd::trace_fetch::{self, pin_for, Pin, PINNED_VERSION};
use pulp_rs::tool_registry;

/// Repo root, two levels up from `experimental/pulp-rs`.
fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(|p| p.parent())
        .expect("experimental/pulp-rs has a repo root two levels up")
        .to_path_buf()
}

fn load_registry() -> tool_registry::ToolRegistry {
    let path = repo_root()
        .join("tools")
        .join("packages")
        .join("tool-registry.json");
    tool_registry::load(&path).expect("real tool-registry.json parses")
}

/// Registry platform key → Perfetto artifact platform key. The registry speaks
/// Pulp's `current_platform_key()` vocabulary (`macOS-arm64`); the fetcher
/// speaks Perfetto's artifact-path vocabulary (`mac-arm64`).
fn perfetto_key(registry_key: &str) -> &'static str {
    match registry_key {
        "macOS-arm64" => "mac-arm64",
        "macOS-x64" => "mac-amd64",
        "Linux-x64" => "linux-amd64",
        "Linux-arm64" => "linux-arm64",
        "Windows-x64" => "windows-amd64",
        other => panic!("unmapped trace-processor registry platform key: {other}"),
    }
}

#[test]
fn registry_entry_parses_with_expected_shape() {
    let reg = load_registry();
    let tp = reg
        .tools
        .get("trace-processor")
        .expect("registry defines trace-processor");
    assert_eq!(tp.id, "trace-processor");
    assert_eq!(tp.install_method, "binary_download");
    assert_eq!(tp.pinned_version, PINNED_VERSION);
    // One source per desktop host the fetcher pins.
    assert_eq!(tp.binary_sources.len(), trace_fetch::PINS.len());
}

#[test]
fn registry_urls_match_the_verified_fetcher_pins() {
    let reg = load_registry();
    let tp = &reg.tools["trace-processor"];

    for (registry_key, source) in &tp.binary_sources {
        let pin: &Pin =
            pin_for(perfetto_key(registry_key)).expect("mapped Perfetto key has a pin");
        // The registry stores the URL with a literal `${version}`; render it
        // the way the installer would and compare to the fetcher's canonical URL.
        let rendered = source.url_template.replace("${version}", PINNED_VERSION);
        assert_eq!(
            rendered,
            trace_fetch::pin_url(pin),
            "trace-processor URL for {registry_key} drifted from cmd::trace_fetch"
        );
        assert_eq!(&source.binary_name, &pin.filename);
    }
}

#[test]
fn every_fetcher_pin_has_a_registry_source() {
    let reg = load_registry();
    let tp = &reg.tools["trace-processor"];

    // Reverse direction: no pinned platform is missing from the tool surface.
    let mapped: Vec<&'static str> = tp
        .binary_sources
        .keys()
        .map(|k| perfetto_key(k))
        .collect();
    for pin in trace_fetch::PINS {
        assert!(
            mapped.contains(&pin.platform),
            "fetcher pins {} but the registry has no matching source",
            pin.platform
        );
    }
}
