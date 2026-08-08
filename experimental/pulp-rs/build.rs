fn main() {
    println!("cargo:rerun-if-env-changed=PULP_RS_BUILD_VERSION");

    let manifest_dir = std::path::PathBuf::from(
        std::env::var_os("CARGO_MANIFEST_DIR").expect("Cargo sets CARGO_MANIFEST_DIR"),
    );
    let product_matrix = manifest_dir.join("../../tools/scripts/release_product_matrix.json");
    println!("cargo:rerun-if-changed={}", product_matrix.display());
    let matrix = std::fs::read_to_string(&product_matrix)
        .unwrap_or_else(|error| panic!("could not read {}: {error}", product_matrix.display()));
    let broker_floor = json_string_field(&matrix, "control_broker_floor")
        .expect("release product matrix must declare control_broker_floor");
    println!("cargo:rustc-env=PULP_CONTROL_BROKER_FLOOR={broker_floor}");

    if let Ok(version) = std::env::var("PULP_RS_BUILD_VERSION") {
        if !version.is_empty() {
            println!("cargo:rustc-env=PULP_RS_BUILD_VERSION={version}");
        }
    }
}

fn json_string_field<'a>(json: &'a str, field: &str) -> Option<&'a str> {
    let key = format!("\"{field}\"");
    let after_key = json.split_once(&key)?.1;
    let after_colon = after_key.split_once(':')?.1.trim_start();
    let value = after_colon.strip_prefix('"')?;
    value.split_once('"').map(|(value, _)| value)
}
