#!/usr/bin/env bash
# macpro-governor — capacity admission for CI VMs on a Proxmox host.
#
# Mirrors the tiered model in Pulp's CLAUDE.md. Tier 0 is already enforced by the
# hypervisor (per-VM cores / cpulimit / cpuunits / balloon=0), so a VM physically
# cannot exceed its slice. This is Tier 1: refuse to START work that would
# oversubscribe the host in aggregate.
#
# The bound that matters on a shared host is a SHARE, never the machine's core
# count. Reserve headroom for the hypervisor so a saturated build cannot make the
# host itself unresponsive — an unreachable host is worse than a queued job,
# because you lose the ability to diagnose it.
#
#   macpro-governor.sh status          # report capacity and current commitment
#   macpro-governor.sh can-start <id>  # exit 0 if VM <id> fits, 1 if it would not
#   macpro-governor.sh can-start-new <cores> <mem_mb>
#
# Every value is derived from the machine it runs on; nothing here names a
# particular host's core count or memory size. The MACPRO_GOVERNOR_* variables
# override a derived value when an operator has a measured reason to.
#
# A refusal always names the axis that refused (cpu, memory, or disk) on stdout,
# so the caller's log says why a clone did not start rather than only that it
# did not.
set -uo pipefail

QM="${MACPRO_GOVERNOR_QM:-qm}"
LVS="${MACPRO_GOVERNOR_LVS:-lvs}"

total_cores=$(nproc)
total_mem_mb=$(free -m | awk '/Mem:/{print $2}')

# Hypervisor reserve: one thread in six (at least 2) and one eighth of memory
# (at least 4 GiB). On a 12-thread 32 GiB host that is 2 threads + 4 GiB.
derived_reserve_cores=$(( (total_cores + 5) / 6 ))
[ "$derived_reserve_cores" -ge 2 ] || derived_reserve_cores=2
derived_reserve_mem_mb=$(( total_mem_mb / 8 ))
[ "$derived_reserve_mem_mb" -ge 4096 ] || derived_reserve_mem_mb=4096
HOST_RESERVE_CORES="${MACPRO_GOVERNOR_RESERVE_CORES:-$derived_reserve_cores}"
HOST_RESERVE_MEM_MB="${MACPRO_GOVERNOR_RESERVE_MEM_MB:-$derived_reserve_mem_mb}"

# The two axes are NOT symmetric, and treating them the same is a real mistake:
#
#   Memory is a HARD limit. Overcommitting it means the OOM killer picks a victim
#   mid-link, which surfaces as a corrupt or truncated object file and reads like
#   a compiler bug. Never overcommit.
#
#   CPU is a SOFT limit. vCPU oversubscription is normal on a hypervisor — build
#   jobs are bursty, not pinned, and Tier 0 already bounds each VM via cpulimit
#   and cpuunits. Refusing all overcommit just leaves threads idle while work
#   queues. 1.5x is conservative for compile workloads.
CPU_OVERCOMMIT_NUM=3
CPU_OVERCOMMIT_DEN=2

# Disk is the third axis. Linked clones and the VMs that build in them write
# into the thin pool; a pool that fills puts every thin volume on it into
# read-only or error state at once, which corrupts the running guests rather
# than refusing new ones. Refuse before that, with headroom for the guests that
# are already running to keep writing. Metadata exhaustion is the worse failure,
# so its threshold is lower.
THIN_POOL="${MACPRO_GOVERNOR_THIN_POOL:-pve/data}"
MAX_DATA_PERCENT="${MACPRO_GOVERNOR_MAX_DATA_PERCENT:-85}"
MAX_META_PERCENT="${MACPRO_GOVERNOR_MAX_META_PERCENT:-75}"

avail_cores=$(( (total_cores - HOST_RESERVE_CORES) * CPU_OVERCOMMIT_NUM / CPU_OVERCOMMIT_DEN ))
phys_cores=$(( total_cores - HOST_RESERVE_CORES ))
avail_mem_mb=$(( total_mem_mb - HOST_RESERVE_MEM_MB ))

# Sum cores+memory of VMs that are actually RUNNING. A stopped VM's allocation
# costs nothing, so counting it would under-admit.
committed_cores=0; committed_mem_mb=0; running_list=""
for id in $("$QM" list 2>/dev/null | awk 'NR>1 && $3=="running" {print $1}'); do
    c=$("$QM" config "$id" 2>/dev/null | awk '/^cores:/{print $2}')
    m=$("$QM" config "$id" 2>/dev/null | awk '/^memory:/{print $2}')
    committed_cores=$(( committed_cores + ${c:-0} ))
    committed_mem_mb=$(( committed_mem_mb + ${m:-0} ))
    running_list="${running_list} ${id}(${c:-?}c/${m:-?}M)"
done

free_cores=$(( avail_cores - committed_cores ))
free_mem_mb=$(( avail_mem_mb - committed_mem_mb ))

# Thin-pool fill as integer percentages. An unreadable pool is a refusal, not a
# pass: admission that cannot see the disk must not assume it is empty.
data_pct=""; meta_pct=""
pool_line="$("$LVS" --noheadings --nosuffix -o data_percent,metadata_percent "$THIN_POOL" 2>/dev/null | head -1)"
if [ -n "$pool_line" ]; then
    read -r data_raw meta_raw <<< "$pool_line"
    data_pct="${data_raw%%.*}"
    meta_pct="${meta_raw%%.*}"
    [[ "$data_pct" =~ ^[0-9]+$ && "$meta_pct" =~ ^[0-9]+$ ]] || { data_pct=""; meta_pct=""; }
fi

# refusal_reasons <cores> <mem_mb>: prints one reason per refusing axis, nothing
# when the request fits.
refusal_reasons() {
    local c="$1" m="$2"
    [ "$c" -le "$free_cores" ] \
        || echo "cpu: need ${c} vCPU, ${free_cores} free of ${avail_cores} lease-able"
    [ "$m" -le "$free_mem_mb" ] \
        || echo "memory: need ${m}M, ${free_mem_mb}M free of ${avail_mem_mb}M lease-able"
    if [ -z "$data_pct" ]; then
        echo "disk: thin pool ${THIN_POOL} fill is unreadable"
    else
        [ "$data_pct" -lt "$MAX_DATA_PERCENT" ] \
            || echo "disk: thin pool ${THIN_POOL} data ${data_pct}% >= ${MAX_DATA_PERCENT}%"
        [ "$meta_pct" -lt "$MAX_META_PERCENT" ] \
            || echo "disk: thin pool ${THIN_POOL} metadata ${meta_pct}% >= ${MAX_META_PERCENT}%"
    fi
}

# admit <label> <cores> <mem_mb>
admit() {
    local label="$1" c="$2" m="$3" reasons
    reasons="$(refusal_reasons "$c" "$m")"
    if [ -z "$reasons" ]; then
        echo "ADMIT ${label}(${c}c/${m}M) — free ${free_cores}c/${free_mem_mb}M, thin pool ${data_pct}%/${meta_pct}%"
        exit 0
    fi
    echo "REFUSE ${label}(${c}c/${m}M)"
    printf '%s\n' "$reasons" | sed 's/^/  because /'
    exit 1
}

case "${1:-status}" in
  status)
    echo "host:      ${total_cores} threads, ${total_mem_mb}M"
    echo "reserved:  ${HOST_RESERVE_CORES} threads, ${HOST_RESERVE_MEM_MB}M (hypervisor)"
    echo "lease-able: ${avail_cores} vCPU (${phys_cores} physical x ${CPU_OVERCOMMIT_NUM}/${CPU_OVERCOMMIT_DEN} overcommit), ${avail_mem_mb}M (no mem overcommit)"
    echo "running:  ${running_list:-none}"
    echo "committed: ${committed_cores} threads, ${committed_mem_mb}M"
    echo "FREE:      ${free_cores} threads, ${free_mem_mb}M"
    echo "disk:      ${THIN_POOL} data ${data_pct:-?}% (max ${MAX_DATA_PERCENT}), metadata ${meta_pct:-?}% (max ${MAX_META_PERCENT})"
    # Load is advisory: high load with cores free means a VM is thrashing, which
    # admission control cannot see from allocation alone.
    echo "loadavg:   $(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null || echo unknown)"
    ;;
  can-start)
    id="${2:?usage: can-start <vmid>}"
    c=$("$QM" config "$id" 2>/dev/null | awk '/^cores:/{print $2}')
    m=$("$QM" config "$id" 2>/dev/null | awk '/^memory:/{print $2}')
    [ -z "${c:-}" ] && { echo "no such VM: $id" >&2; exit 2; }
    admit "$id " "$c" "$m"
    ;;
  can-start-new)
    c="${2:?usage: can-start-new <cores> <mem_mb>}"; m="${3:?}"
    admit "" "$c" "$m"
    ;;
  *) echo "usage: $0 {status|can-start <vmid>|can-start-new <cores> <mem_mb>}" >&2; exit 2 ;;
esac
