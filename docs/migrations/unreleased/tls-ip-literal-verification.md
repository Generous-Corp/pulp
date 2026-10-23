---
breaking = true
summary = "HTTPS requests to an IP-literal host now verify the server certificate chain"
---

## Breaking changes
- `pulp::runtime::http_request` (and `http_get` / `http_post` / `http_download`) now verifies the certificate chain for an `https://<ip-address>/` URL exactly as it does for a hostname URL. Previously the mbedTLS backend ran no chain check for an IP-literal host, so any self-signed certificate that listed the IP in its subjectAltName was accepted; such a connection now fails with `Connection failed: SSL server verification failed`.

## Migration steps
- Address TLS servers by a name the certificate carries, or issue the server a certificate chained to a root the client host trusts; the client has no per-request trust override.
- A development server with a self-signed certificate must be reached over plain `http://` on loopback or trusted at the OS level.
