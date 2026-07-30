//! Rust-owned lifecycle dispatch for SHA-256-verified archive tools.
//!
//! These tools cannot use the generic `pulp-cpp` archive delegate because
//! their committed verification metadata and transactional install rules live
//! in Rust. Keep the install/update ownership here so adding another verified
//! archive does not add another tool-id branch to the main `tool` orchestrator.

use std::io::Write;

use crate::error::{CliError, Result};
use crate::tool_registry::{ToolDescriptor, ToolRegistry};
use crate::tool_version;

use super::report_active_version;

const CHROME_FOR_TESTING: &str = "chrome-for-testing";

/// Resolve aliases while reserving Chrome's canonical spelling for its
/// code-owned verified lifecycle, even if checkout-local registry metadata
/// removes that descriptor and reuses the spelling as an alias.
pub(super) fn canonical_id(registry: &ToolRegistry, requested: &str) -> String {
    if requested == CHROME_FOR_TESTING {
        requested.to_owned()
    } else {
        registry.canonical_id(requested)
    }
}

/// Ensure a delegated `install --all` cannot opt Chrome into the generic
/// registry-driven downloader.
pub(super) fn validate_all(registry: &ToolRegistry) -> Result<()> {
    let Some(chrome) = registry.tools.get(CHROME_FOR_TESTING) else {
        return Ok(());
    };
    if chrome.install_method != "verified_archive" || !chrome.explicit_install_only {
        return Err(CliError::BadUsage(
            "refusing `pulp tool install --all`: chrome-for-testing must remain \
             an explicit-only verified archive"
                .to_owned(),
        ));
    }
    Ok(())
}

/// Run a named verified-archive install, or return `None` for tools owned by a
/// different lifecycle.
pub(super) fn install(
    id: Option<&str>,
    tool: Option<&ToolDescriptor>,
    force: bool,
    version: Option<&str>,
    out: &mut impl Write,
) -> Option<Result<i32>> {
    // Chrome's verified installer is a code-owned security boundary. Dispatch
    // by canonical id before consulting checkout-controlled registry metadata
    // so deleting or relabelling the descriptor cannot downgrade it to the
    // generic C++ archive downloader.
    if id == Some(CHROME_FOR_TESTING) {
        return Some(
            crate::cmd::chrome_for_testing::validate_requested_version(version)
                .and_then(|()| crate::cmd::chrome_for_testing::install_pinned(force, out)),
        );
    }

    let tool = tool?;
    (tool.install_method == "verified_archive").then(|| Err(unsupported(&tool.id)))
}

/// Run a named verified-archive update, or return `None` for tools owned by a
/// different lifecycle.
pub(super) fn update(
    tool: &ToolDescriptor,
    version: Option<&str>,
    out: &mut impl Write,
) -> Option<Result<i32>> {
    if tool.id == CHROME_FOR_TESTING {
        return Some(update_chrome(tool, version, out));
    }

    (tool.install_method == "verified_archive").then(|| Err(unsupported(&tool.id)))
}

/// Remove Chrome through its verified lifecycle's config/importer home rather
/// than the distinct generic managed-tool home used on Windows.
pub(super) fn uninstall(id: &str) -> Option<Result<Option<std::path::PathBuf>>> {
    (id == CHROME_FOR_TESTING).then(crate::cmd::chrome_for_testing::uninstall_pinned)
}

fn update_chrome(
    tool: &ToolDescriptor,
    version: Option<&str>,
    out: &mut impl Write,
) -> Result<i32> {
    crate::cmd::chrome_for_testing::validate_requested_version(version)?;
    let _ = tool_version::clear_override(&tool.id)?;
    report_active_version(&tool.id, &tool_version::resolve_active(tool), out)?;
    crate::cmd::chrome_for_testing::install_pinned(true, out)
}

fn unsupported(id: &str) -> CliError {
    CliError::Other(format!(
        "verified-archive tool '{id}' has no Rust lifecycle implementation"
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    #[test]
    fn ignores_tools_owned_by_another_lifecycle() {
        let tool = ToolDescriptor {
            id: "uv".to_owned(),
            install_method: "binary_download".to_owned(),
            ..ToolDescriptor::default()
        };
        assert!(install(Some("uv"), Some(&tool), false, None, &mut Vec::new()).is_none());
        assert!(update(&tool, None, &mut Vec::new()).is_none());
    }

    #[test]
    fn rejects_unimplemented_verified_archive_lifecycle() {
        let tool = ToolDescriptor {
            id: "future-verified-tool".to_owned(),
            install_method: "verified_archive".to_owned(),
            ..ToolDescriptor::default()
        };
        let error = install(Some(&tool.id), Some(&tool), false, None, &mut Vec::new())
            .expect("verified archives are owned here")
            .unwrap_err();
        assert!(format!("{error}").contains("has no Rust lifecycle implementation"));
    }

    #[test]
    fn chrome_id_cannot_be_downgraded_or_removed_by_registry_metadata() {
        let downgraded = ToolDescriptor {
            id: CHROME_FOR_TESTING.to_owned(),
            install_method: "binary_download".to_owned(),
            ..ToolDescriptor::default()
        };
        for descriptor in [Some(&downgraded), None] {
            let error = install(
                Some(CHROME_FOR_TESTING),
                descriptor,
                false,
                Some("unverified-version"),
                &mut Vec::new(),
            )
            .expect("Chrome id is always owned by the verified lifecycle")
            .unwrap_err();
            assert!(format!("{error}").contains("only accepts the verified registry pin"));
        }
    }

    #[test]
    fn chrome_spelling_cannot_be_reassigned_as_an_alias() {
        let mut registry = ToolRegistry::default();
        registry.tools = BTreeMap::from([(
            "unverified-browser".to_owned(),
            ToolDescriptor {
                id: "unverified-browser".to_owned(),
                aliases: vec![CHROME_FOR_TESTING.to_owned()],
                install_method: "binary_download".to_owned(),
                ..ToolDescriptor::default()
            },
        )]);
        assert_eq!(
            canonical_id(&registry, CHROME_FOR_TESTING),
            CHROME_FOR_TESTING
        );
    }

    #[test]
    fn install_all_requires_chrome_to_stay_verified_and_explicit() {
        let mut registry = ToolRegistry::default();
        registry.tools.insert(
            CHROME_FOR_TESTING.to_owned(),
            ToolDescriptor {
                id: CHROME_FOR_TESTING.to_owned(),
                install_method: "verified_archive".to_owned(),
                explicit_install_only: true,
                ..ToolDescriptor::default()
            },
        );
        assert!(validate_all(&registry).is_ok());

        registry
            .tools
            .get_mut(CHROME_FOR_TESTING)
            .unwrap()
            .explicit_install_only = false;
        assert!(validate_all(&registry).is_err());
        registry
            .tools
            .get_mut(CHROME_FOR_TESTING)
            .unwrap()
            .explicit_install_only = true;
        registry
            .tools
            .get_mut(CHROME_FOR_TESTING)
            .unwrap()
            .install_method = "binary_download".to_owned();
        assert!(validate_all(&registry).is_err());
    }
}
