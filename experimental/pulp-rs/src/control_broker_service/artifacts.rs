use super::{path_text, ControlBrokerServiceError, ServicePaths, CONTROL_BROKER_LABEL};

pub(super) fn fingerprint(contents: &[u8]) -> String {
    let mut value = 0xcbf2_9ce4_8422_2325_u64;
    for byte in contents {
        value ^= u64::from(*byte);
        value = value.wrapping_mul(0x0000_0100_0000_01b3);
    }
    format!("fnv1a64:{value:016x}")
}

pub(super) fn render_plist(paths: &ServicePaths) -> Result<String, ControlBrokerServiceError> {
    let broker = xml_escape(&path_text(&paths.broker)?);
    let stdout = xml_escape(&path_text(&paths.stdout_log)?);
    let stderr = xml_escape(&path_text(&paths.stderr_log)?);
    Ok(format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n\
<plist version=\"1.0\">\n\
<dict>\n\
  <key>Label</key>\n  <string>{CONTROL_BROKER_LABEL}</string>\n\
  <key>ProgramArguments</key>\n  <array>\n    <string>{broker}</string>\n  </array>\n\
  <key>RunAtLoad</key>\n  <true/>\n\
  <key>KeepAlive</key>\n  <true/>\n\
  <key>ThrottleInterval</key>\n  <integer>10</integer>\n\
  <key>StandardOutPath</key>\n  <string>{stdout}</string>\n\
  <key>StandardErrorPath</key>\n  <string>{stderr}</string>\n\
</dict>\n\
</plist>\n"
    ))
}

pub(super) fn xml_escape(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}
