#!/usr/bin/env bash
set -euo pipefail

# Report how far Lucia is from the last OpenEngine commit it merged, per module.
# The base is the merge base of the target and the upstream ref. It is correct
# only when each import is a real merge commit (docs/development/openengine-imports.mdx).
# Exit 0 when the upstream ref is merged into the target, 1 when it is not.
#   ./Tools/Scripts/report-openengine-divergence.sh [upstream-ref] [target-ref] [rows]
#   ./Tools/Scripts/report-openengine-divergence.sh upstream/main origin/main 30

upstreamRef="${1:-upstream/main}"
targetRef="${2:-HEAD}"
rowCount="${3:-25}"

git rev-parse --verify --quiet "$upstreamRef^{commit}" >/dev/null || {
    echo "Unknown upstream ref '$upstreamRef'. Run 'git fetch upstream' or pass an OpenEngine commit." >&2
    exit 2
}
git rev-parse --verify --quiet "$targetRef^{commit}" >/dev/null || {
    echo "Unknown target ref '$targetRef'." >&2
    exit 2
}

base="$(git merge-base "$targetRef" "$upstreamRef")"
pending="$(git rev-list --count "$targetRef..$upstreamRef")"

echo "Upstream: $(git log -1 --format='%h %cs %s' "$upstreamRef" | cut -c1-100)"
echo "Target:   $(git log -1 --format='%h %cs %s' "$targetRef" | cut -c1-100)"
echo "Base:     $(git log -1 --format='%h %cs %s' "$base" | cut -c1-100)"
echo "Upstream commits not merged into the target: $pending"
echo

git diff --no-renames --numstat "$base" "$targetRef" | awk -F '\t' -v rows="$rowCount" '
    # Binary files report "-" for both counts: count the file, not lines.
    {
        added = ($1 == "-") ? 0 : $1
        deleted = ($2 == "-") ? 0 : $2
        n = split($3, part, "/")
        if (part[1] == "Engine" && part[2] == "Modules" && n > 3) key = part[1] "/" part[2] "/" part[3]
        else if (part[1] == "Apps" && n > 3) key = part[1] "/" part[2] "/" part[3]
        else if (n > 2) key = part[1] "/" part[2]
        else key = part[1]
        files[key]++
        lines[key] += added + deleted
        totalFiles++
        totalLines += added + deleted
    }
    END {
        printf "%-48s %8s %10s\n", "Area", "Files", "Lines"
        sortCommand = "sort -t \"\t\" -k3,3nr -k1,1 | head -n " rows " | awk -F \"\t\" '\''{ printf \"%-48s %8d %10d\\n\", $1, $2, $3 }'\''"
        for (key in files) printf "%s\t%d\t%d\n", key, files[key], lines[key] | sortCommand
        close(sortCommand)
        printf "%-48s %8d %10d\n", "Total", totalFiles, totalLines
    }'

if [ "$pending" -ne 0 ]; then
    echo
    echo "'$upstreamRef' is not merged into '$targetRef'. After an import, merge the OpenEngine commit with a merge commit, never a squash."
    exit 1
fi
