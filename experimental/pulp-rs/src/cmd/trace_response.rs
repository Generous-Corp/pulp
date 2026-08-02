//! Typed access to inspector trace response objects.

/// Parsed inspector response used by trace command rendering and readiness checks.
pub(crate) struct TraceResponse(serde_json::Value);

impl TraceResponse {
    /// Parse a JSON object response.
    pub(crate) fn parse(json: &str) -> Option<Self> {
        let value: serde_json::Value = serde_json::from_str(json).ok()?;
        value.is_object().then_some(Self(value))
    }

    /// Return a string field when the response contains one with the requested key.
    pub(crate) fn string(&self, key: &str) -> Option<&str> {
        self.0.get(key)?.as_str()
    }

    /// Return a boolean field when the response contains one with the requested key.
    pub(crate) fn boolean(&self, key: &str) -> Option<bool> {
        self.0.get(key)?.as_bool()
    }
}

#[cfg(test)]
mod tests {
    use super::TraceResponse;

    #[test]
    fn reads_string_fields_and_json_escapes() {
        let response = TraceResponse::parse(
            r#"{"out_path":"/tmp/pulp-1.pftrace","explanation":"the \"lead\" voice"}"#,
        )
        .unwrap();

        assert_eq!(response.string("out_path"), Some("/tmp/pulp-1.pftrace"));
        assert_eq!(response.string("explanation"), Some("the \"lead\" voice"));
        assert_eq!(response.string("missing"), None);
    }

    #[test]
    fn reads_boolean_fields() {
        let response = TraceResponse::parse(r#"{"compiled_in":true,"active":false}"#).unwrap();

        assert_eq!(response.boolean("compiled_in"), Some(true));
        assert_eq!(response.boolean("active"), Some(false));
        assert_eq!(response.boolean("missing"), None);
    }

    #[test]
    fn rejects_malformed_and_non_object_responses() {
        assert!(TraceResponse::parse("{").is_none());
        assert!(TraceResponse::parse("[]").is_none());
    }
}
