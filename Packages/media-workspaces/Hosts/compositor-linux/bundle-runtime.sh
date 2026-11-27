#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

if [[ $# -ne 1 || ! -f "$1/libLuciaMediaWorkspace.so" ]]; then
    printf 'Usage: %s <staged Compositor host directory>\n' "$0" >&2
    exit 2
fi

host=$(cd "$1" && pwd)
runtime="$host/runtime"
dependencies=$(ldd "$host/libLuciaMediaWorkspace.so")
if [[ "$dependencies" == *'not found'* ]]; then
    printf '%s\n' "$dependencies" >&2
    exit 1
fi

mkdir -p "$runtime/licenses"
manifest="$runtime/Dependencies.txt"
printf 'Compositor imaging runtime\n\n' > "$manifest"

while read -r soname arrow library _; do
    [[ "$arrow" == '=>' && "$library" == /* ]] || continue
    case "$soname" in
        libgif.so.*|liblensfun.so.*|libOpenEXR*.so.*|libImath*.so.*|\
        libIex*.so.*|libIlmThread*.so.*|liblcms2.so.*|libjpeg.so.*|libtiff.so.*|\
        libheif.so.*|libraw.so.*|libLerc.so.*|libjbig.so.*|\
        libdeflate.so.*|libwebp.so.*|libsharpyuv.so.*)
            ;;
        *) continue ;;
    esac

    install -m 755 "$library" "$runtime/$soname"
    patchelf --set-rpath '$ORIGIN' "$runtime/$soname"

    # Package metadata records the exact distribution source of the shipped bytes.
    package=''
    if command -v dpkg-query >/dev/null; then
        resolved=$(realpath "$library")
        ownership=$(dpkg-query -S "$resolved" 2>/dev/null || true)
        package=${ownership%%: /*}
    fi
    if [[ -n "$package" ]]; then
        printf '%s\t' "$soname" >> "$manifest"
        dpkg-query -W -f='${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n' "$package" >> "$manifest"
        notice="/usr/share/doc/${package%%:*}/copyright"
        if [[ -f "$notice" ]]; then
            install -m 644 "$notice" "$runtime/licenses/${package%%:*}.txt"
        fi
    else
        printf '%s\t%s\n' "$soname" "$(basename "$(realpath "$library")")" >> "$manifest"
    fi
done <<< "$dependencies"

patchelf --set-rpath '$ORIGIN:$ORIGIN/runtime' "$host/libLuciaMediaWorkspace.so"
