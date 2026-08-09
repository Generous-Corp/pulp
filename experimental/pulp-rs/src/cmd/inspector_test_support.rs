use crate::cmd::inspector::InspectorTalker;
use crate::error::Result;

pub(crate) fn s(strs: &[&str]) -> Vec<String> {
    strs.iter().map(|x| (*x).to_owned()).collect()
}

/// Test-only talker that records calls and returns canned
/// responses. Lets us exercise `dispatch` without a real `pulp-cpp` binary.
pub(crate) struct RecordingTalker {
    responses: std::cell::RefCell<Vec<String>>,
    pub(crate) calls: std::cell::RefCell<Vec<(String, String, Option<String>)>>,
}

impl RecordingTalker {
    pub(crate) fn new(responses: Vec<&str>) -> Self {
        Self {
            responses: std::cell::RefCell::new(responses.into_iter().map(str::to_owned).collect()),
            calls: std::cell::RefCell::new(Vec::new()),
        }
    }

    fn record_call(&self, method: &str, params: &str, instance_id: Option<&str>) -> Result<String> {
        self.calls.borrow_mut().push((
            method.to_owned(),
            params.to_owned(),
            instance_id.map(str::to_owned),
        ));
        let mut responses = self.responses.borrow_mut();
        if responses.is_empty() {
            Ok("{}".to_owned())
        } else {
            Ok(responses.remove(0))
        }
    }
}

impl InspectorTalker for RecordingTalker {
    fn call(&self, method: &str, params: &str, instance_id: Option<&str>) -> Result<String> {
        self.record_call(method, params, instance_id)
    }
}
