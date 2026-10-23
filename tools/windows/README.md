# Local Windows ARM desktop helper

`open-utm-rdp.sh` is a local, non-runner demo helper. It starts one verified
UTM guest, repairs RDP through that guest's authenticated SSH connection, and
opens the desktop with SDL FreeRDP. It does not discover a guest by position or
silently fall back to another VM.

Before first use, verify the guest identity and forwarding values on the host,
then save the non-secret values in `~/.config/pulp/windows-utm.env`:

```sh
/Applications/UTM.app/Contents/MacOS/utmctl list
mkdir -p -m 700 ~/.config/pulp
cat >~/.config/pulp/windows-utm.env <<'EOF'
SPECTR_WINDOWS_UTM_UUID='<verified-uuid>'
SPECTR_WINDOWS_SSH_PORT='<verified-local-ssh-port>'
SPECTR_WINDOWS_RDP_PORT='<verified-local-rdp-port>'
EOF
```

The launcher reads that file automatically; `SPECTR_WINDOWS_UTM_CONFIG` can
select another machine-local file. Explicit environment variables override its
values.

Keep the admin credential in
`~/.config/pulp/secrets/spectr-windows-utm-admin` with mode `0600` and do not
put it in this repository. The helper refuses an absent UUID, so a stale
configuration cannot start the wrong guest.

The loopback SSH connection uses the guest key supplied by the local image and
does not pin a host key in this repository. Treat the image and its key as a
local trusted test fixture; do not reuse these scripts for an untrusted remote
host without adding host-key verification.

Run:

```sh
./tools/windows/open-utm-rdp.sh
# Optional local safety check (does not start a guest):
./tools/windows/test_helpers.sh
```

The guest is a bounded development/demo VM and is not a GitHub runner.
