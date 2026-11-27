#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    printf 'Usage: %s <new scratch directory> <workspace host directory>\n' "$0" >&2
    exit 2
fi
adapter=$(cd "$(dirname "$0")" && pwd)
mkdir "$1"
scratch=$(cd "$1" && pwd)
mkdir -p "$2"
destination=$(cd "$2" && pwd)
revision=e49b0b57fd130ad7a565b598b01353407a5fd59b

git init --quiet "$scratch/source"
git -C "$scratch/source" fetch --quiet --depth 1 https://github.com/ClaudiuJitea/CompositorLX.git "$revision"
git -C "$scratch/source" checkout --quiet --detach FETCH_HEAD
git -C "$scratch/source" apply --unidiff-zero "$adapter/lucia-host.patch"

# CMake checks the pinned archive digest before extraction. The mirror carries the same bytes.
onnx_urls='https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-linux-x64-1.23.2.tgz;https://huggingface.co/csukuangfj/onnxruntime-libs/resolve/82dd633f912b8dfe8a6544a369baac5bfbd88c1e/1.23.2/onnxruntime-linux-x64-1.23.2.tgz'
cmake -S "$scratch/source" -B "$scratch/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DONNXRUNTIME_ARCHIVE="$onnx_urls" -DLUCIA_COMPOSITOR_HOST_SOURCE="$adapter/LuciaCompositor.cpp"
cmake --build "$scratch/build" --target LuciaMediaWorkspace
staged="$scratch/staged/Compositor"
cmake --install "$scratch/build" --component LuciaWorkspace --prefix "$staged"
bash "$adapter/bundle-runtime.sh" "$staged"
cmake -E copy_directory "$staged" "$destination/Compositor"
