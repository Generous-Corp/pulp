---
breaking = true
summary = "OnlineActivation refuses http:// servers by default, POSTs /status with a JSON body, and accepts only an exact {\"status\":\"valid\"|\"expired\"} verdict"
---

## Breaking changes

- `OnlineActivation::activate`, `deactivate` and `check_status` refuse a
  `server_url` that is not `https://`. A plain-http URL fails before any
  request is sent (`activate` returns nothing, `deactivate` returns false,
  `check_status` returns `LicenseStatus::NotFound`). The new
  `OnlineActivationOptions::allow_insecure_http` re-admits `http://` for a
  loopback test server.
- `check_status` sends `POST /status` with the JSON body
  `{"key":"...","machine":"..."}` instead of `GET /status?key=...&machine=...`.
  A license server that only handles the GET form stops answering.
- The `/status` response is parsed, not searched: it must be a JSON object
  whose `status` member is exactly `"valid"` or `"expired"`. A body such as
  `{"valid":true}` or any other text containing the word is
  `LicenseStatus::InvalidSignature`.
- Every request field (serial, product id, license key, machine id) is
  JSON-escaped. A serial containing `"` or `\` arrives intact instead of
  corrupting or extending the object.

## Migration steps

- Point clients at an `https://` origin. For a local test server pass
  `OnlineActivationOptions{.allow_insecure_http = true}` as the trailing
  argument.
- On the server, accept `POST /status` with a JSON body and reply with
  `{"status":"valid"}` or `{"status":"expired"}`; any other value denies.
- Servers that already escaped incoming JSON need no change; servers that
  pattern-matched the raw body should parse it.
