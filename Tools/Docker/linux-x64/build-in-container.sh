#!/usr/bin/env bash
# The container half of build.sh: provisions vcpkg at the pinned commit, configures the linux-x64
# preset in Release and builds Editor and Player, then McpServer, then GameEngineCli (the last two
# run npm in mcp/ and must not run at the same time), and runs the packager when one is mounted.
# Prints the seconds each step took.
set -euo pipefail

memMb=$(awk '/MemTotal/ { print int($2 / 1024) }' /proc/meminfo)
jobs=${GE_JOBS:-$(( memMb / 1900 ))}
jobs=$(( jobs < 1 ? 1 : jobs > $(nproc) ? $(nproc) : jobs ))
echo "memory ${memMb} MB, $(nproc) CPUs: ${jobs} compile jobs, 1 link job"

Step() {
    local name=$1 start=$SECONDS
    shift
    "$@"
    echo "step ${name}: $(( SECONDS - start )) s"
}

ProvisionVcpkg() {
    local pin
    pin=$(sed -n 's/^set(GE_VCPKG_COMMIT "\([0-9a-f]*\)")$/\1/p' cmake/VcpkgPin.cmake)
    [ -d /vcpkg/.git ] || git clone --quiet https://github.com/microsoft/vcpkg.git /vcpkg
    if [ "$(git -C /vcpkg rev-parse HEAD)" != "$pin" ]; then
        git -C /vcpkg fetch --quiet origin
        git -C /vcpkg checkout --quiet --detach "$pin"
        rm -f /vcpkg/vcpkg
    fi
    [ -x /vcpkg/vcpkg ] || /vcpkg/bootstrap-vcpkg.sh -disableMetrics
}

# npm and dotnet write into the bind-mounted source tree as root; hand those files back, also when
# a step fails.
ReturnOwnership() {
    find /src -xdev -uid 0 -exec chown -h "$GE_HOST_UID:$GE_HOST_GID" {} +
}
trap ReturnOwnership EXIT

export VCPKG_ROOT=/vcpkg VCPKG_MAX_CONCURRENCY=$jobs
export VCPKG_BINARY_SOURCES="clear;files,/vcpkg-cache,readwrite"
start=$SECONDS
Step vcpkg ProvisionVcpkg
Step configure cmake --preset linux-x64 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_JOB_POOLS=link=1 -DCMAKE_JOB_POOL_LINK=link
Step editor-player cmake --build build/linux-x64 --target Editor Player -j "$jobs"
Step mcp-server cmake --build build/linux-x64 --target McpServer
Step cli cmake --build build/linux-x64 --target GameEngineCli
echo "build total: $(( SECONDS - start )) s"
if [ -n "${GE_PACKAGER:-}" ]; then
    Step package python3 -I "$GE_PACKAGER" --platform linux-x64 --worktree /src --version "$(cat VERSION)" \
        --readme /readme.txt --out /out --cli build/linux-x64/bin/Release/gameenginecli
    chown -R "$GE_HOST_UID:$GE_HOST_GID" /out
fi
