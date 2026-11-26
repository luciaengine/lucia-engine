#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s <new output directory>\n' "$0" >&2
    exit 2
fi

output="$1"
script_directory="$(cd "$(dirname "$0")" && pwd)"
if [[ -e "$output" ]]; then
    printf 'The output directory already exists: %s\n' "$output" >&2
    exit 2
fi

version=0.17.0
if [[ "$(bun --version)" != 1.3.14 ]]; then
    printf 'Building OpenPencil %s requires Bun 1.3.14.\n' "$version" >&2
    exit 2
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
curl -fLsS --retry 2 "https://codeload.github.com/open-pencil/open-pencil/tar.gz/refs/tags/v$version" \
    -o "$scratch/source.tar.gz"
curl -fLsS --retry 2 "https://registry.npmjs.org/@open-pencil/html-engine/-/html-engine-$version.tgz" \
    -o "$scratch/html-engine.tgz"
(
    cd "$scratch"
    shasum -a 256 -c <<'SUMS'
cdd2f9cbfb277c7ec3bad076836461631b2a31966b56a6fe41cc37db4254ec43  source.tar.gz
34d72bdebf4af59ec3452f49b5a7fdcade48c471a023419413b38dce1fff0309  html-engine.tgz
SUMS
    mkdir source
    tar -xf source.tar.gz -C source --strip-components 1
    cd source
    patch -p1 < "$script_directory/openpencil-lucia-save.patch"
    patch -p1 < "$script_directory/openpencil-shape-preview.patch"
    patch -p1 < "$script_directory/openpencil-share-style.patch"
    patch -p1 < "$script_directory/openpencil-quit-prompt.patch"
    bun install --frozen-lockfile
    tar -xf ../html-engine.tgz -C packages/html-engine --strip-components 1 \
        package/wasm/html-engine.wasm package/wasm/html-engine.wasm.sha256 package/wasm/THIRD-PARTY-NOTICES.md
    (cd packages/html-engine && bun scripts/wasm.ts --check)
    bun run build:packages
    VITE_OPENPENCIL_CLOUD=off bun x vite build
    cp LICENSE dist/LICENSE
)
mkdir -p "$(dirname "$output")"
mv "$scratch/source/dist" "$output"
