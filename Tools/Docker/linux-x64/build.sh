#!/usr/bin/env bash
# Builds the Linux x64 Editor, Player, MCP server and gameenginecli in Release inside the image
# from Tools/Docker/Dockerfile.linux-dev, on any host with Docker (an arm64 host runs the x86-64
# image under emulation), and optionally zips the Editor package with the release packager.
#
#   Tools/Docker/linux-x64/build.sh [--jobs N] [--cache DIR] \
#       [--packager package_editor_zip.py --readme README.txt --out DIR]
#
# The source tree is bind-mounted at /src. The build tree (build/linux-x64) and the vcpkg checkout
# live in Docker volumes, not on the bind mount: compiling through a host file share is several
# times slower. vcpkg is cloned at the commit cmake/VcpkgPin.cmake pins, and its binary cache is
# the host folder --cache (default ~/.cache/ge-vcpkg-linux-x64), so a second build, or a fresh
# volume, restores the ports instead of compiling them. A run resumes where the last one stopped:
# after an out-of-memory kill (exit 137), rerun with a lower --jobs.
#
# --jobs defaults to one compile per 1.9 GB of the Docker VM's memory (a Release GCC compile of
# the larger engine files peaks near that), at most one per CPU; links run one at a time.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../.." && pwd)
jobs=""
cache="${HOME}/.cache/ge-vcpkg-linux-x64"
packager="" readme="" out=""
while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) jobs=$2; shift 2 ;;
        --cache) cache=$2; shift 2 ;;
        --packager) packager=$2; shift 2 ;;
        --readme) readme=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        *) echo "unknown argument: $1 (see the comment at the top of $0)" >&2; exit 2 ;;
    esac
done
if [ -n "$packager" ] && { [ -z "$readme" ] || [ -z "$out" ]; }; then
    echo "--packager needs --readme and --out" >&2; exit 2
fi

image=ge-linux-x64-build
docker build --platform linux/amd64 -f "$here/../Dockerfile.linux-dev" -t "$image" "$here/.."

# One build volume per source tree; the vcpkg checkout is shared by every tree on this host.
treeId=$(printf '%s' "$src" | cksum | cut -d' ' -f1)
mkdir -p "$cache"
mounts=(-v "$src:/src" -v "ge-linux-x64-build-$treeId:/src/build" -v ge-linux-x64-vcpkg:/vcpkg -v "$cache:/vcpkg-cache")
# A git worktree's .git is a file naming the main repository's .git by its host path: mount that
# directory at the same path, read-only, so git works inside the container.
if [ -f "$src/.git" ]; then
    common=$(git -C "$src" rev-parse --path-format=absolute --git-common-dir)
    mounts+=(-v "$common:$common:ro")
fi
env=(-e "GE_JOBS=$jobs" -e "GE_HOST_UID=$(id -u)" -e "GE_HOST_GID=$(id -g)")
if [ -n "$packager" ]; then
    mkdir -p "$out"
    mounts+=(-v "$(cd "$(dirname "$packager")" && pwd):/packager:ro" -v "$(cd "$(dirname "$readme")" && pwd)/$(basename "$readme"):/readme.txt:ro" -v "$(cd "$out" && pwd):/out")
    env+=(-e "GE_PACKAGER=/packager/$(basename "$packager")")
fi

docker run --rm --platform linux/amd64 "${mounts[@]}" "${env[@]}" -w /src "$image" \
    bash Tools/Docker/linux-x64/build-in-container.sh
