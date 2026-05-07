//! Build-time identity for the Rust CLI.
//!
//! Release binaries are built through CMake, which passes the Pulp SDK
//! version into Cargo as `PULP_RS_BUILD_VERSION`. Direct `cargo build`
//! prototype runs keep falling back to the crate's package version.

/// Version baked into this binary at build time.
#[must_use]
pub fn baked_cli_version() -> &'static str {
    option_env!("PULP_RS_BUILD_VERSION").unwrap_or(env!("CARGO_PKG_VERSION"))
}

/// CLI version string, honoring the `PULP_RS_CLI_VERSION` override so
/// tests can pin a version without rebuilding the binary.
#[must_use]
pub fn cli_version_string() -> String {
    if let Ok(v) = std::env::var("PULP_RS_CLI_VERSION") {
        if !v.is_empty() {
            return v;
        }
    }
    baked_cli_version().to_owned()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::ENV_LOCK;

    #[test]
    fn cli_version_prefers_env_override() {
        let _guard = ENV_LOCK
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let prev = std::env::var_os("PULP_RS_CLI_VERSION");
        std::env::set_var("PULP_RS_CLI_VERSION", "9.8.7");
        assert_eq!(cli_version_string(), "9.8.7");
        match prev {
            Some(v) => std::env::set_var("PULP_RS_CLI_VERSION", v),
            None => std::env::remove_var("PULP_RS_CLI_VERSION"),
        }
    }

    #[test]
    fn cli_version_falls_back_to_baked_version() {
        let _guard = ENV_LOCK
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let prev = std::env::var_os("PULP_RS_CLI_VERSION");
        std::env::remove_var("PULP_RS_CLI_VERSION");
        assert_eq!(cli_version_string(), baked_cli_version());
        if let Some(v) = prev {
            std::env::set_var("PULP_RS_CLI_VERSION", v);
        }
    }
}
