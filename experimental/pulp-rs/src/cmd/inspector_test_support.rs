use crate::cmd::inspector::InspectorTalker;
use crate::error::Result;

pub(crate) fn s(strs: &[&str]) -> Vec<String> {
    strs.iter().map(|x| (*x).to_owned()).collect()
}

/// Test-only talker that records calls and returns canned
/// responses. Lets us exercise `dispatch` without a real
/// `pulp-cpp` binary or a live inspector.
pub(crate) struct RecordingTalker {
    responses: std::cell::RefCell<Vec<String>>,
    pub(crate) calls: std::cell::RefCell<Vec<(u16, String, String)>>,
    pub(crate) selections: std::cell::RefCell<Vec<Option<crate::cmd::inspector::SessionSelection>>>,
}

impl RecordingTalker {
    pub(crate) fn new(responses: Vec<&str>) -> Self {
        Self {
            responses: std::cell::RefCell::new(responses.into_iter().map(str::to_owned).collect()),
            calls: std::cell::RefCell::new(Vec::new()),
            selections: std::cell::RefCell::new(Vec::new()),
        }
    }

    fn record_call(
        &self,
        port: u16,
        selection: Option<crate::cmd::inspector::SessionSelection>,
        method: &str,
        params: &str,
    ) -> Result<String> {
        self.calls
            .borrow_mut()
            .push((port, method.to_owned(), params.to_owned()));
        self.selections.borrow_mut().push(selection);
        let mut responses = self.responses.borrow_mut();
        if responses.is_empty() {
            Ok("{}".to_owned())
        } else {
            Ok(responses.remove(0))
        }
    }
}

impl InspectorTalker for RecordingTalker {
    fn call(&self, port: u16, method: &str, params: &str) -> Result<String> {
        self.record_call(port, None, method, params)
    }

    fn call_selected(
        &self,
        port: u16,
        session_id: &str,
        instance_id: &str,
        publication_id: &str,
        method: &str,
        params: &str,
    ) -> Result<String> {
        self.record_call(
            port,
            Some(crate::cmd::inspector::SessionSelection {
                session_id: session_id.to_owned(),
                instance_id: instance_id.to_owned(),
                publication_id: publication_id.to_owned(),
            }),
            method,
            params,
        )
    }
}
