#!/usr/bin/env bash
set -euo pipefail

release_tag=${1:?usage: upload-release-asset.sh <vX.Y.Z tag>}
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"

if ! gh release view "$release_tag" --repo "$GITHUB_REPOSITORY" >/dev/null; then
    echo "ERROR: expected GitHub Release $release_tag does not exist" >&2
    exit 1
fi

artifact_list=$("$(dirname "${BASH_SOURCE[0]}")/check-release-artifacts.sh" "${release_tag#v}")
mapfile -t artifacts <<<"$artifact_list"

# --clobber so a re-run after a failed upload replaces what it left behind
# rather than erroring on the half that already arrived.
gh release upload "$release_tag" "${artifacts[@]}" \
    --repo "$GITHUB_REPOSITORY" \
    --clobber
