#!/bin/bash
# Linker launcher for PULP_DEV_SHARED_UI (see PulpDevSharedUi.cmake).
#
#   pulp_dev_shared_link.sh dylib  <record>          <linker> <args...>
#   pulp_dev_shared_link.sh module <dylib> <record>  <linker> <args...>
#
# CMake runs it in front of the real link command of the shared runtime and of
# every plug-in module that links against it.
#
# dylib:  each format-independent libpulp-*.a on the link line is force-loaded
#         once, so the dylib exports all of it. The adapter archives
#         (libpulp-format*, libpulp-standalone*) and the VST3 / AudioUnit SDKs
#         are dropped: they stay static in each module. Every other archive
#         resolves normally. After a successful link the basenames of the
#         force-loaded archives are written to <record>, and those of the other
#         archives to <record>.resolved; each is rewritten only when it changes.
# module: the dylib is inserted ahead of the first archive, so the linker
#         resolves everything it exports from it and never loads a second copy
#         of a member, and an absolute rpath to its directory is added. Every
#         force-loaded archive is dropped. The link is first tried without the
#         dylib's third-party archives as well: they cost link time and the
#         module almost never needs a member the dylib lacks. When that link
#         fails (a symbol only such an archive defines), it is redone with
#         them, and the module is marked so later links of it skip the attempt.
#         A link that allows undefined symbols never takes the shortcut,
#         because it could succeed while leaving a symbol unbound.
#
# Archives are matched by basename because the two links can name the same
# archive by different relative paths (Unix Makefiles links from each target's
# own directory). Runs under macOS /bin/bash 3.2 and spawns no subprocess
# besides the linker, so it adds a few milliseconds to a link.

mode=$1
shift
case $mode in
dylib)
    record=$1
    shift
    ;;
module)
    dylib=$1
    record=$2
    shift 2
    ;;
*)
    echo "pulp_dev_shared_link.sh: unknown mode '$mode'" >&2
    exit 2
    ;;
esac
if [ $# -eq 0 ]; then
    echo "pulp_dev_shared_link.sh: no link command" >&2
    exit 2
fi

nl=$'\n'

# Reads a record file into a newline-delimited set: $nl<base>$nl<base>$nl...
read_set() {
    set_value=$nl
    if [ -f "$1" ]; then
        while IFS= read -r line; do
            set_value=$set_value$line$nl
        done <"$1"
    fi
}

# Writes `$2` to file `$1` unless it already holds exactly that.
write_if_changed() {
    read_set "$1"
    if [ "${set_value#"$nl"}" != "$2" ]; then
        printf '%s' "$2" >"$1.tmp.$$" && mv -f "$1.tmp.$$" "$1" || exit 1
        changed=1
    fi
}

if [ "$mode" = dylib ]; then
    args=()
    loaded=$nl
    resolved=$nl
    for a in "$@"; do
        path=
        case $a in
        @*.rsp)
            echo "pulp_dev_shared_link.sh: response file '$a' is not supported" >&2
            exit 2
            ;;
        -Wl,-force_load,*) path=${a#-Wl,-force_load,} ;;
        *.a) path=$a ;;
        esac
        if [ -n "$path" ]; then
            base=${path##*/}
            case $base in
            libpulp-format*.a | libpulp-standalone*.a | libvst3-sdk.a | libausdk.a)
                continue
                ;;
            libpulp-*.a)
                case $loaded in
                *"$nl$base$nl"*) ;;
                *)
                    loaded=$loaded$base$nl
                    args+=("-Wl,-force_load,$path")
                    ;;
                esac
                continue
                ;;
            esac
            case $resolved in
            *"$nl$base$nl"*) ;;
            *) resolved=$resolved$base$nl ;;
            esac
        fi
        args+=("$a")
    done
    if ! "${args[@]}"; then
        echo "pulp_dev_shared_link.sh: the development-only shared Pulp runtime did not link." \
            "Build with static plug-in links instead: \`pulp dev --no-dev-shared\`," \
            "\`pulp build\`, or -DPULP_DEV_SHARED_UI=OFF." >&2
        exit 1
    fi
    changed=
    write_if_changed "$record" "${loaded#"$nl"}"
    write_if_changed "$record.resolved" "${resolved#"$nl"}"
    # A module marked as needing the third-party archives was marked against
    # the previous runtime; let it try the shortcut again.
    if [ -n "$changed" ] && [ -d "$record.full-link" ]; then
        rm -rf "$record.full-link"
    fi
    exit 0
fi

if [ ! -f "$record" ]; then
    echo "pulp_dev_shared_link.sh: $record is missing; the pulp_dev_shared_ui target must link first" >&2
    exit 1
fi
read_set "$record"
shared=$set_value
read_set "$record.resolved"
resolved=$set_value

full=()
fast=()
inserted=
output=
shortcut=1
previous=
for a in "$@"; do
    if [ "$previous" = -o ]; then
        output=$a
    fi
    previous=$a
    path=
    case $a in
    @*.rsp)
        echo "pulp_dev_shared_link.sh: response file '$a' is not supported" >&2
        exit 2
        ;;
    *dynamic_lookup*) shortcut= ;;
    -Wl,-force_load,*) path=${a#-Wl,-force_load,} ;;
    *.a) path=$a ;;
    esac
    if [ -n "$path" ]; then
        if [ -z "$inserted" ]; then
            full+=("$dylib")
            fast+=("$dylib")
            inserted=1
        fi
        case $shared in
        *"$nl${path##*/}$nl"*) continue ;;
        esac
        full+=("$a")
        case $resolved in
        *"$nl${path##*/}$nl"*) ;;
        *) fast+=("$a") ;;
        esac
        continue
    fi
    full+=("$a")
    fast+=("$a")
done
if [ -z "$inserted" ]; then
    full+=("$dylib")
    fast+=("$dylib")
fi
full+=("-Wl,-rpath,${dylib%/*}")
fast+=("-Wl,-rpath,${dylib%/*}")

marker_name=${output//\//_}
marker="$record.full-link/${marker_name:-unnamed}"
if [ -n "$shortcut" ] && [ ! -f "$marker" ]; then
    # The shortcut's diagnostics are replayed only when it succeeds; on
    # failure the full link reports whatever is really wrong.
    log="$record.fast-link.${marker_name:-unnamed}.log"
    if "${fast[@]}" 2>"$log"; then
        while IFS= read -r line; do
            printf '%s\n' "$line" >&2
        done <"$log"
        exit 0
    fi
    if [ ! -d "$record.full-link" ]; then
        mkdir -p "$record.full-link" || exit 1
    fi
    "${full[@]}" || exit $?
    : >"$marker"
    exit 0
fi
exec "${full[@]}"
