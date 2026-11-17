#!/bin/bash
# The Linux final check: fetch a ref from GitHub, build the whole tree (DebugFast, tests on) in
# a podman container made from Tools/LinuxTestRunner/Containerfile, run every test suite, and
# write a summary. It runs on the Linux machine alone; no other machine is needed.
#
# Usage:      run-tests.sh [ref]                          (default: main)
# Background: nohup run-tests.sh main > "$LUCIA_CI_DIR/last.out" 2>&1 &
# Result:     $LUCIA_CI_DIR/results/<time>-<sha>/summary.txt  (also $LUCIA_CI_DIR/results/latest)
#
# Settings, from the environment (the defaults are the layout of the first test machine):
#   LUCIA_CI_DIR         the mirror, the lock and the results       ($HOME/lucia-ci)
#   LUCIA_SRC_DIR        the source tree the container builds in    ($HOME/lucia-rtf/src)
#   LUCIA_VCPKG_DIR      the vcpkg checkout                         ($HOME/lucia/vcpkg)
#   LUCIA_VCPKG_ARCHIVES the vcpkg binary cache                     ($HOME/lucia/vcpkg-archives)
#   LUCIA_IMAGE          the container image                        (localhost/lucia-linux-dev:latest)
#   LUCIA_REPO_URL       the repository to fetch from               (the Lucia repository on GitHub)
#   LUCIA_JOBS           parallel compiles                          (6)
# A DebugFast compile of the larger engine files needs about 2 GB. 6 jobs fit a 15 GB machine;
# more jobs end in out-of-memory kills of the Editor compiles.
set -u
REF="${1:-main}"
CI="${LUCIA_CI_DIR:-$HOME/lucia-ci}"
SRC="${LUCIA_SRC_DIR:-$HOME/lucia-rtf/src}"
VCPKG="${LUCIA_VCPKG_DIR:-$HOME/lucia/vcpkg}"
VCPKG_ARCHIVES="${LUCIA_VCPKG_ARCHIVES:-$HOME/lucia/vcpkg-archives}"
IMAGE="${LUCIA_IMAGE:-localhost/lucia-linux-dev:latest}"
REPO="${LUCIA_REPO_URL:-https://github.com/luciaengine/lucia-engine.git}"
JOBS="${LUCIA_JOBS:-6}"

mkdir -p "$CI/results" || exit 1
exec 9>"$CI/lock"
flock -n 9 || { echo "Another run is active (lock $CI/lock). Stop."; exit 1; }
# A second build container on the same source tree would overwrite this run's files.
for c in $(podman ps -q); do podman inspect "$c" --format "{{range .Mounts}}{{.Source}} {{end}}"; done | grep -q "$SRC" \
  && { echo "A build container already uses $SRC. Wait for it to end."; exit 1; }

# 1. Fetch the ref into a local mirror and export it.
[ -d "$CI/repo.git" ] || git clone -q --bare --filter=blob:none "$REPO" "$CI/repo.git" || exit 1
git -C "$CI/repo.git" fetch -q origin "+refs/heads/*:refs/heads/*" || exit 1
SHA=$(git -C "$CI/repo.git" rev-parse --short=9 "$REF") || { echo "Unknown ref $REF"; exit 1; }
RUN="$(date +%Y%m%d-%H%M)-$SHA"
OUT="$CI/results/$RUN"; mkdir -p "$OUT/xml"
ln -sfn "$OUT" "$CI/results/latest"
echo "ref $REF sha $SHA start $(date)" | tee "$OUT/summary.txt"

# 2. Sync the source into the build tree. rsync writes only the changed files, so the build is incremental.
rm -rf "$CI/stage" && mkdir -p "$CI/stage"
git -C "$CI/repo.git" archive "$SHA" | tar -x -C "$CI/stage" || exit 1
rsync -rlc --delete --exclude=/build "$CI/stage/" "$SRC/" || exit 1
rm -rf "$CI/stage"

# 3. Configure, build and test inside the container. The quoted heredoc keeps host variables
# out of the script; the container gets LUCIA_JOBS from the environment.
cat > "$OUT/inner.sh" <<"INNER"
#!/bin/bash
cd /lucia/src
export VCPKG_ROOT=/lucia/vcpkg
O=/lucia/out
configure() {
  cmake --preset linux-x64-local -DVCPKG_OVERLAY_PORTS=/lucia/src/cmake/linux-ports -DCMAKE_BUILD_TYPE=DebugFast \
    -DBUILD_TESTING=ON -DVCPKG_MANIFEST_INSTALL=ON -DCMAKE_TOOLCHAIN_FILE=/lucia/vcpkg/scripts/buildsystems/vcpkg.cmake \
    > $O/configure.log 2>&1
}
configure
rc=$?; echo "configure $rc" > $O/status
if [ $rc -ne 0 ] && grep -q "delete .* and reconfigure" $O/configure.log; then
  echo "dependency set changed: clean rebuild" >> $O/status
  rm -rf build/linux-x64-local
  configure
  rc=$?; echo "configure $rc" >> $O/status
fi
[ $rc -eq 0 ] || exit 1
cd build/linux-x64-local
ninja -k 0 -j "$LUCIA_JOBS" > $O/build.log 2>&1
echo "build $?" >> $O/status
for t in bin/DebugFast/Tests/*; do
  [ -x "$t" ] && [ -f "$t" ] || continue
  n=$(basename "$t")
  case "$n" in *.so|*.so.*|FakeAgentCli|ProcessStreamProbe) continue;; *Test*) ;; *) continue;; esac
  GE_VK_VALIDATION=1 timeout 2400 "./$t" --gtest_output=xml:$O/xml/$n.xml > $O/$n.log 2>&1
  echo "run $n $?" >> $O/status
done
echo done >> $O/status
INNER
chmod +x "$OUT/inner.sh"
podman run --rm --device /dev/dri -e LUCIA_JOBS="$JOBS" \
  -v "$SRC":/lucia/src -v "$VCPKG":/lucia/vcpkg \
  -v "$VCPKG_ARCHIVES":/root/.cache/vcpkg/archives \
  -v "$OUT":/lucia/out "$IMAGE" bash /lucia/out/inner.sh

# 4. Summary: build errors and one line per suite.
{
  echo "--- status"; cat "$OUT/status" 2>/dev/null
  echo "--- build errors (first 40)"; grep -E "error:|FAILED:" "$OUT/build.log" 2>/dev/null | sort -u | head -40
  echo "--- suites (tests failures disabled errors)"
  for x in "$OUT"/xml/*.xml; do
    [ -f "$x" ] || continue
    printf "%-45s %s\n" "$(basename "$x" .xml)" "$(grep -o "<testsuites [^>]*" "$x" | grep -oE "(tests|failures|disabled|errors)=\"[0-9]+\"" | tr "\n" " ")"
  done
  echo "--- suites with no result file (crash or timeout)"
  grep "^run " "$OUT/status" 2>/dev/null | while read _ n rc; do [ -f "$OUT/xml/$n.xml" ] || echo "$n exit $rc"; done
  echo "end $(date)"
} >> "$OUT/summary.txt"
cat "$OUT/summary.txt"
