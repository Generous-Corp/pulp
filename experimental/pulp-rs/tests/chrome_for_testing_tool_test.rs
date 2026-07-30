use std::fs;
use std::path::Path;
use std::process::Command;

use pulp_rs::cmd::chrome_for_testing::{pin_for, pin_url, PINNED_VERSION, PINS};
use pulp_rs::tool_registry;

fn cft_key(registry_key: &str) -> &'static str {
    match registry_key {
        "macOS-arm64" => "mac-arm64",
        "macOS-x64" => "mac-x64",
        "Linux-x64" => "linux64",
        "Windows-x64" => "win64",
        other => panic!("unexpected Chrome-for-Testing platform {other}"),
    }
}

#[test]
fn registry_and_verified_archive_pins_are_exactly_aligned() {
    let registry = tool_registry::load(Path::new("../../tools/packages/tool-registry.json"))
        .expect("tool registry");
    let tool = &registry.tools["chrome-for-testing"];
    assert_eq!(tool.pinned_version, PINNED_VERSION);
    assert_eq!(tool.install_method, "verified_archive");
    assert!(!tool.bundleable);
    assert!(tool.managed_by_pulp);
    assert!(tool.explicit_install_only);
    assert_eq!(tool.install_scope, "machine");
    assert_eq!(tool.binary_sources.len(), PINS.len());

    for (registry_key, source) in &tool.binary_sources {
        let pin = pin_for(cft_key(registry_key)).expect("mapped platform pin");
        assert_eq!(source.url_template, pin_url(pin));
        assert_eq!(source.archive_format, "zip");
        assert_eq!(
            source.binary_name,
            Path::new(pin.executable)
                .file_name()
                .unwrap()
                .to_string_lossy()
        );
    }
}

#[test]
fn registry_does_not_claim_an_unpublished_linux_arm64_archive() {
    let registry = tool_registry::load(Path::new("../../tools/packages/tool-registry.json"))
        .expect("tool registry");
    let tool = &registry.tools["chrome-for-testing"];
    assert!(!tool.binary_sources.contains_key("Linux-arm64"));
}

#[test]
fn independent_verification_record_covers_every_committed_pin() {
    let record_path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../tools/packages/chrome-for-testing-verification.md");
    let record = fs::read_to_string(record_path).unwrap();
    assert!(record.contains(PINNED_VERSION));
    for pin in PINS {
        assert!(
            record.contains(&pin_url(pin)),
            "missing URL for {}",
            pin.platform
        );
        assert!(
            record.contains(pin.sha256),
            "missing SHA-256 for {}",
            pin.platform
        );
    }
}

#[test]
fn chrome_specific_uninstall_works_outside_a_checkout() {
    let scratch = tempfile::tempdir().unwrap();
    let home = scratch.path().join("pulp-home");
    let managed = home.join("tools/chrome-for-testing");
    let sentinel = home.join("tools/keep-me/sentinel");
    fs::create_dir_all(managed.join("151.0.7922.47/fixture")).unwrap();
    fs::write(managed.join("151.0.7922.47/fixture/browser"), b"fixture").unwrap();
    fs::create_dir_all(sentinel.parent().unwrap()).unwrap();
    fs::write(&sentinel, b"keep").unwrap();

    let result = Command::new(env!("CARGO_BIN_EXE_pulp"))
        .current_dir(scratch.path())
        .env("PULP_HOME", &home)
        .env("PULP_UPDATE_CHECK_DISABLED", "1")
        .env_remove("PULP_USE_CPP")
        .args(["tool", "uninstall", "chrome-for-testing"])
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "stdout: {}\nstderr: {}",
        String::from_utf8_lossy(&result.stdout),
        String::from_utf8_lossy(&result.stderr)
    );
    let stdout = String::from_utf8_lossy(&result.stdout);
    assert!(stdout.contains("Uninstalled chrome-for-testing"));
    assert!(stdout.contains(&managed.to_string_lossy()[..]));
    assert!(!managed.exists());
    assert_eq!(fs::read(sentinel).unwrap(), b"keep");
}

#[test]
fn chrome_uninstall_ignores_empty_pulp_home_and_uses_platform_default() {
    let scratch = tempfile::tempdir().unwrap();
    let user_home = scratch.path().join("user-home");
    let user_profile = scratch.path().join("user-profile");
    let local_app_data = scratch.path().join("local-app-data");
    let home = if cfg!(windows) {
        user_profile.join(".pulp")
    } else {
        user_home.join(".pulp")
    };
    let managed = home.join("tools/chrome-for-testing");
    fs::create_dir_all(managed.join("151.0.7922.47/fixture")).unwrap();
    fs::write(managed.join("151.0.7922.47/fixture/browser"), b"fixture").unwrap();

    let result = Command::new(env!("CARGO_BIN_EXE_pulp"))
        .current_dir(scratch.path())
        .env("PULP_HOME", "")
        .env("HOME", &user_home)
        .env("USERPROFILE", &user_profile)
        .env("LOCALAPPDATA", &local_app_data)
        .env("PULP_UPDATE_CHECK_DISABLED", "1")
        .env_remove("PULP_USE_CPP")
        .args(["tool", "uninstall", "chrome-for-testing"])
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "stdout: {}\nstderr: {}",
        String::from_utf8_lossy(&result.stdout),
        String::from_utf8_lossy(&result.stderr)
    );
    assert!(!managed.exists());
    assert!(
        !scratch.path().join("tools").exists(),
        "empty PULP_HOME must never resolve to a cwd-relative tools directory"
    );
}
