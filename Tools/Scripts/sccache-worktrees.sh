#!/usr/bin/env bash
set -euo pipefail

# Restart the sccache server with every worktree of this repository as a base
# directory, so an object one worktree compiled is a cache hit in another.
# sccache strips a base directory from paths before it hashes a compile, but it
# reads SCCACHE_BASEDIRS only when the server starts: a server already running
# keeps the list it started with. Run this after `git worktree add`.
# Direct mode is off: with base directories, sccache 0.18 direct mode returned
# another branch's object for a source that #includes a .cpp by relative path
# (Engine/Modules/Scripting/Source/PathResolver.cpp). Preprocessor mode hashes
# the whole preprocessed source, so a hit always matches what would compile.
#   ./Tools/Scripts/sccache-worktrees.sh

command -v sccache >/dev/null || { echo "sccache is not installed (brew install sccache)" >&2; exit 1; }

basedirs=""
while IFS= read -r line; do
    case "$line" in
        "worktree "*) basedirs="${basedirs:+$basedirs:}${line#worktree }" ;;
    esac
done < <(git -C "$(dirname "$0")" worktree list --porcelain)

sccache --stop-server >/dev/null 2>&1 || true
SCCACHE_DIRECT=false SCCACHE_BASEDIRS="$basedirs" sccache --start-server
sccache --show-stats | grep -E "Base directories|direct/preprocessor"
