#!/usr/bin/env bash
# Configure the Mac Pro's three no-uplink Proxmox CI bridges without touching
# its management bridge. Changes are explicit, reversible, and verified before
# an automatic runner may be enabled.
set -euo pipefail

INTERFACES_FILE=/etc/network/interfaces.d/pulp-ci-isolation
SYSCTL_FILE=/etc/sysctl.d/99-pulp-ci-isolation.conf
STATE_FILE=/var/lib/pulp/ci-host-network.state
LOCK_FILE=/var/lock/pulp-ci-host-network.lock
VMID_LOCK_FILE=/var/lock/pulp-ephemeral-vmid.lock
MANAGEMENT_BRIDGE=vmbr0
BRIDGES=(vmbr-ci200 vmbr-ci201 vmbr-ci202)
CONTROLLER_ADDRESSES=(10.240.200.1/30 10.240.201.1/30 10.240.202.1/30)
SUBNETS=(10.240.200.0/30 10.240.201.0/30 10.240.202.0/30)
MODE="${1:-}"

usage() {
    cat <<'EOF'
Usage: configure-proxmox-ci-network.sh --dry-run|--verify|--apply|--rollback

  --dry-run   inspect prerequisites and print the files/commands; change nothing
  --verify    fail unless the managed files and live topology are exact
  --apply     install and activate the exact topology, then verify it
  --rollback  remove only the exact managed topology and restore forwarding
EOF
}

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

render_interfaces() {
    cat <<'EOF'
# Managed by Pulp configure-proxmox-ci-network.sh. Do not edit in place.
# These bridges have no physical ports and never join vmbr0 or the LAN.
auto vmbr-ci200
iface vmbr-ci200 inet static
    address 10.240.200.1/30
    bridge-ports none
    bridge-stp off
    bridge-fd 0
    post-up /usr/local/sbin/configure-proxmox-ci-network --ensure-nat
    pre-down /usr/local/sbin/configure-proxmox-ci-network --remove-nat vmbr-ci200

auto vmbr-ci201
iface vmbr-ci201 inet static
    address 10.240.201.1/30
    bridge-ports none
    bridge-stp off
    bridge-fd 0
    post-up /usr/local/sbin/configure-proxmox-ci-network --ensure-nat
    pre-down /usr/local/sbin/configure-proxmox-ci-network --remove-nat vmbr-ci201

auto vmbr-ci202
iface vmbr-ci202 inet static
    address 10.240.202.1/30
    bridge-ports none
    bridge-stp off
    bridge-fd 0
    post-up /usr/local/sbin/configure-proxmox-ci-network --ensure-nat
    pre-down /usr/local/sbin/configure-proxmox-ci-network --remove-nat vmbr-ci202
EOF
}

render_sysctl() {
    cat <<'EOF'
# Managed by Pulp configure-proxmox-ci-network.sh.
net.ipv4.ip_forward = 1
EOF
}

require_host() {
    [ "$(uname -s)" = Linux ] || die "this helper is for the Proxmox Linux host"
    for command_name in ip iptables iptables-save ifup ifdown sysctl flock; do
        command -v "$command_name" >/dev/null || die "$command_name is required"
    done
    [ -d /etc/pve ] || die "/etc/pve is absent; refusing to configure a non-Proxmox host"
    [ -f /etc/network/interfaces ] || die "/etc/network/interfaces is absent"
    grep -Eq '^[[:space:]]*source[[:space:]]+/etc/network/interfaces\.d/\*' \
        /etc/network/interfaces \
        || die "/etc/network/interfaces does not source interfaces.d"
}

management_signature() {
    local addresses default_route
    addresses="$(ip -o -4 addr show dev "$MANAGEMENT_BRIDGE" scope global \
        | awk '{print $4}' | sort)"
    default_route="$(ip -o -4 route show default dev "$MANAGEMENT_BRIDGE")"
    [ -n "$addresses" ] || die "$MANAGEMENT_BRIDGE has no global IPv4 address"
    [ -n "$default_route" ] || die "$MANAGEMENT_BRIDGE has no default IPv4 route"
    printf '%s\n%s\n' "$addresses" "$default_route"
}

file_is_exact() {
    local path="$1" renderer="$2" expected metadata
    [ -f "$path" ] && [ ! -L "$path" ] || return 1
    metadata="$(stat -c '%u:%g:%a' -- "$path" 2>/dev/null)" || return 1
    [ "$metadata" = 0:0:644 ] || return 1
    expected="$(mktemp)"
    "$renderer" >"$expected"
    cmp -s "$expected" "$path"
    local result=$?
    rm -f "$expected"
    return "$result"
}

state_is_valid() {
    local metadata
    [ -f "$STATE_FILE" ] && [ ! -L "$STATE_FILE" ] || return 1
    metadata="$(stat -c '%u:%g:%a' -- "$STATE_FILE" 2>/dev/null)" || return 1
    [ "$metadata" = 0:0:600 ] || return 1
    [ "$(wc -l <"$STATE_FILE" | tr -d ' ')" = 1 ] || return 1
    grep -Eq '^previous_ip_forward=[01]$' "$STATE_FILE"
}

refuse_conflicts() {
    local index bridge expected_address live_addresses ports
    if [ -e "$INTERFACES_FILE" ] && ! file_is_exact "$INTERFACES_FILE" render_interfaces; then
        die "$INTERFACES_FILE exists but is not the exact managed file"
    fi
    if [ -e "$SYSCTL_FILE" ] && ! file_is_exact "$SYSCTL_FILE" render_sysctl; then
        die "$SYSCTL_FILE exists but is not the exact managed file"
    fi
    for index in "${!BRIDGES[@]}"; do
        bridge="${BRIDGES[$index]}"
        expected_address="${CONTROLLER_ADDRESSES[$index]}"
        [ -e "/sys/class/net/$bridge" ] || continue
        [ -d "/sys/class/net/$bridge/bridge" ] \
            || die "$bridge exists but is not a Linux bridge"
        ports=("/sys/class/net/$bridge/brif/"*)
        [ ! -e "${ports[0]}" ] || die "$bridge has an attached port"
        live_addresses="$(ip -o -4 addr show dev "$bridge" scope global \
            | awk '{print $4}')"
        [ "$live_addresses" = "$expected_address" ] \
            || die "$bridge has unexpected IPv4 configuration: ${live_addresses:-none}"
    done
}

nat_rule() {
    local operation="$1" subnet="$2" bridge="$3"
    iptables -w 10 -t nat "$operation" POSTROUTING -s "$subnet" \
        -o "$MANAGEMENT_BRIDGE" -m comment \
        --comment "pulp-ci-isolation:$bridge" -j MASQUERADE
}

ensure_nat() {
    local index count
    for index in "${!BRIDGES[@]}"; do
        count="$(count_nat_rules "${SUBNETS[$index]}" "${BRIDGES[$index]}")"
        [ "$count" = 0 ] || [ "$count" = 1 ] \
            || die "${BRIDGES[$index]} has duplicate managed NAT rules"
        if [ "$count" = 0 ]; then
            nat_rule -A "${SUBNETS[$index]}" "${BRIDGES[$index]}"
        fi
        [ "$(count_nat_rules "${SUBNETS[$index]}" "${BRIDGES[$index]}")" = 1 ] \
            || die "cannot prove exact managed NAT for ${BRIDGES[$index]}"
    done
}

lock_host_network_unless_inherited() {
    [ "${PULP_CI_HOST_NETWORK_LOCK_HELD:-0}" = 1 ] && return
    exec 9>"$LOCK_FILE" || die "cannot open $LOCK_FILE"
    flock -w 30 9 || die "timed out waiting for the host-network lock"
}

remove_one_nat() {
    local requested_bridge="$1" index count
    for index in "${!BRIDGES[@]}"; do
        [ "${BRIDGES[$index]}" = "$requested_bridge" ] || continue
        count="$(count_nat_rules "${SUBNETS[$index]}" "$requested_bridge")"
        [[ "$count" =~ ^[0-9]+$ ]] || die "cannot count $requested_bridge NAT rules"
        while [ "$count" -gt 0 ]; do
            nat_rule -D "${SUBNETS[$index]}" "$requested_bridge"
            count=$((count - 1))
        done
        [ "$(count_nat_rules "${SUBNETS[$index]}" "$requested_bridge")" = 0 ] \
            || die "managed NAT rule survived removal for $requested_bridge"
        return
    done
    die "invalid managed bridge for NAT removal"
}

count_nat_rules() {
    local subnet="$1" bridge="$2" rules count status
    rules="$(iptables-save -t nat)" || die "cannot inspect the NAT table"
    count="$(grep -Fc -- \
        "-s $subnet -o $MANAGEMENT_BRIDGE -m comment --comment \"pulp-ci-isolation:$bridge\" -j MASQUERADE" \
        <<<"$rules")" || {
            status=$?
            [ "$status" = 1 ] || die "cannot count managed NAT rules"
        }
    printf '%s\n' "$count"
}

remove_nat() {
    local index count
    for index in "${!BRIDGES[@]}"; do
        count="$(count_nat_rules "${SUBNETS[$index]}" "${BRIDGES[$index]}")"
        [[ "$count" =~ ^[0-9]+$ ]] || die "cannot count managed NAT rules"
        while [ "$count" -gt 0 ]; do
            nat_rule -D "${SUBNETS[$index]}" "${BRIDGES[$index]}"
            count=$((count - 1))
        done
        [ "$(count_nat_rules "${SUBNETS[$index]}" "${BRIDGES[$index]}")" = 0 ] \
            || die "managed NAT rule survived removal for ${BRIDGES[$index]}"
    done
}

verify_live() {
    local index bridge expected_address live_addresses ports count guest_ip route
    file_is_exact "$INTERFACES_FILE" render_interfaces \
        || die "$INTERFACES_FILE is absent or differs from the managed contract"
    file_is_exact "$SYSCTL_FILE" render_sysctl \
        || die "$SYSCTL_FILE is absent or differs from the managed contract"
    state_is_valid || die "$STATE_FILE is absent, insecure, or invalid"
    [ "$(sysctl -n net.ipv4.ip_forward)" = 1 ] || die "IPv4 forwarding is disabled"
    for index in "${!BRIDGES[@]}"; do
        bridge="${BRIDGES[$index]}"
        expected_address="${CONTROLLER_ADDRESSES[$index]}"
        guest_ip="${SUBNETS[$index]%0/30}2"
        [ -d "/sys/class/net/$bridge/bridge" ] || die "$bridge is not active"
        ports=("/sys/class/net/$bridge/brif/"*)
        [ ! -e "${ports[0]}" ] || die "$bridge has an attached port"
        live_addresses="$(ip -o -4 addr show dev "$bridge" scope global \
            | awk '{print $4}')"
        [ "$live_addresses" = "$expected_address" ] \
            || die "$bridge does not own only $expected_address"
        route="$(ip -o -4 route get "$guest_ip")"
        case "$route" in
            *" dev $bridge "*" src ${expected_address%/30} "*) ;;
            *) die "$guest_ip is not routed through $bridge from ${expected_address%/30}" ;;
        esac
        count="$(count_nat_rules "${SUBNETS[$index]}" "$bridge")"
        [ "$count" = 1 ] || die "$bridge must have exactly one managed NAT rule (found $count)"
    done
    management_signature >/dev/null
    printf 'verified: isolated bridges, forwarding, NAT, and %s management route\n' \
        "$MANAGEMENT_BRIDGE"
}

dry_run() {
    require_host
    management_signature >/dev/null
    refuse_conflicts
    printf '%s\n' "Would install $INTERFACES_FILE:"; render_interfaces
    printf '%s\n' "Would install $SYSCTL_FILE:"; render_sysctl
    printf '%s\n' 'Would activate only vmbr-ci200 vmbr-ci201 vmbr-ci202, enable IPv4 forwarding,'
    printf '%s\n' 'and add one source-scoped MASQUERADE rule per /30 through vmbr0.'
    printf '%s\n' 'Would verify the management address/default route remained byte-identical.'
}

apply_network() {
    [ "$EUID" = 0 ] || die "--apply requires root"
    require_host
    refuse_conflicts
    local before after previous_forward tmp_interfaces tmp_sysctl bridge
    exec 9>"$LOCK_FILE" || die "cannot open $LOCK_FILE"
    flock -w 30 9 || die "timed out waiting for the host-network lock"
    exec 8>"$VMID_LOCK_FILE" || die "cannot open $VMID_LOCK_FILE"
    flock -w 300 8 || die "timed out waiting for the VMID allocation lock"
    before="$(management_signature)"
    previous_forward="$(sysctl -n net.ipv4.ip_forward)"
    case "$previous_forward" in 0|1) ;; *) die "unexpected IPv4 forwarding value" ;; esac
    install -d -o root -g root -m 0755 "$(dirname "$STATE_FILE")"
    if [ -e "$STATE_FILE" ]; then
        state_is_valid || die "$STATE_FILE must be exact root-owned mode-0600 state"
    else
        printf 'previous_ip_forward=%s\n' "$previous_forward" \
            | install -o root -g root -m 0600 /dev/stdin "$STATE_FILE"
    fi
    tmp_interfaces="$(mktemp)"; tmp_sysctl="$(mktemp)"
    render_interfaces >"$tmp_interfaces"; render_sysctl >"$tmp_sysctl"
    install -o root -g root -m 0644 "$tmp_interfaces" "$INTERFACES_FILE"
    install -o root -g root -m 0644 "$tmp_sysctl" "$SYSCTL_FILE"
    rm -f "$tmp_interfaces" "$tmp_sysctl"
    sysctl -q -w net.ipv4.ip_forward=1
    export PULP_CI_HOST_NETWORK_LOCK_HELD=1
    for bridge in "${BRIDGES[@]}"; do
        [ -d "/sys/class/net/$bridge/bridge" ] || ifup "$bridge"
    done
    ensure_nat
    verify_live
    after="$(management_signature)"
    [ "$after" = "$before" ] || die "$MANAGEMENT_BRIDGE changed during apply"
}

rollback_network() {
    [ "$EUID" = 0 ] || die "--rollback requires root"
    require_host
    if [ ! -e "$STATE_FILE" ] && [ ! -e "$INTERFACES_FILE" ] \
        && [ ! -e "$SYSCTL_FILE" ]; then
        local clean_bridge clean_index
        for clean_bridge in "${BRIDGES[@]}"; do
            [ ! -e "/sys/class/net/$clean_bridge" ] \
                || die "refusing already-clean rollback: $clean_bridge still exists"
        done
        for clean_index in "${!BRIDGES[@]}"; do
            [ "$(count_nat_rules "${SUBNETS[$clean_index]}" "${BRIDGES[$clean_index]}")" = 0 ] \
                || die "refusing already-clean rollback: a managed NAT rule remains"
        done
        printf 'already rolled back: no managed files, bridges, or NAT rules remain\n'
        return
    fi
    file_is_exact "$INTERFACES_FILE" render_interfaces \
        || die "refusing rollback: $INTERFACES_FILE is absent or modified"
    file_is_exact "$SYSCTL_FILE" render_sysctl \
        || die "refusing rollback: $SYSCTL_FILE is absent or modified"
    state_is_valid || die "refusing rollback: managed state is absent, insecure, or invalid"
    local before after previous_forward bridge ports
    exec 9>"$LOCK_FILE" || die "cannot open $LOCK_FILE"
    flock -w 30 9 || die "timed out waiting for the host-network lock"
    exec 8>"$VMID_LOCK_FILE" || die "cannot open $VMID_LOCK_FILE"
    flock -w 300 8 || die "timed out waiting for the VMID allocation lock"
    previous_forward="$(sed -n 's/^previous_ip_forward=\([01]\)$/\1/p' "$STATE_FILE")"
    [ -n "$previous_forward" ] || die "refusing rollback: managed state is invalid"
    before="$(management_signature)"
    for bridge in "${BRIDGES[@]}"; do
        [ -e "/sys/class/net/$bridge" ] || continue
        [ -d "/sys/class/net/$bridge/bridge" ] || die "$bridge is not a bridge"
        ports=("/sys/class/net/$bridge/brif/"*)
        [ ! -e "${ports[0]}" ] || die "$bridge has an attached port; stop its VM first"
    done
    export PULP_CI_HOST_NETWORK_LOCK_HELD=1
    for bridge in "${BRIDGES[@]}"; do
        [ ! -e "/sys/class/net/$bridge" ] || ifdown "$bridge"
    done
    remove_nat
    sysctl -q -w "net.ipv4.ip_forward=$previous_forward"
    after="$(management_signature)"
    [ "$after" = "$before" ] || die "$MANAGEMENT_BRIDGE changed during rollback"
    rm -f -- "$INTERFACES_FILE" "$SYSCTL_FILE" "$STATE_FILE"
    printf 'rolled back: isolated bridges and NAT removed; IPv4 forwarding restored to %s\n' \
        "$previous_forward"
}

main() {
    case "$MODE" in
        --dry-run) dry_run ;;
        --verify) require_host; verify_live ;;
        --apply) apply_network ;;
        --rollback) rollback_network ;;
        --ensure-nat)
            [ "$EUID" = 0 ] || die "--ensure-nat requires root"
            require_host
            lock_host_network_unless_inherited
            ensure_nat
            ;;
        --remove-nat)
            [ "$EUID" = 0 ] || die "--remove-nat requires root"
            [ "$#" = 2 ] || die "--remove-nat requires one managed bridge"
            require_host
            lock_host_network_unless_inherited
            remove_one_nat "$2"
            ;;
        -h|--help) usage ;;
        *) usage >&2; exit 64 ;;
    esac
}

if [ "${BASH_SOURCE[0]:-$0}" = "$0" ]; then
    main "$@"
fi
