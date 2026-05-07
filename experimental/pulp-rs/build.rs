fn main() {
    println!("cargo:rerun-if-env-changed=PULP_RS_BUILD_VERSION");

    if let Ok(version) = std::env::var("PULP_RS_BUILD_VERSION") {
        if !version.is_empty() {
            println!("cargo:rustc-env=PULP_RS_BUILD_VERSION={version}");
        }
    }
}
