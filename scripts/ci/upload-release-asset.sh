#!/usr/bin/env bash
set -euo pipefail

release_tag=${1:?usage: upload-release-asset.sh <vX.Y.Z tag>}
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"

if ! gh release view "$release_tag" --repo "$GITHUB_REPOSITORY" >/dev/null; then
    echo "ERROR: expected GitHub Release $release_tag does not exist" >&2
    exit 1
fi

version=${release_tag#v}
artifacts=(
    "build/coax-${version}-win64-setup.exe"
    "build/coax-${version}-win64.zip"
    "build/coax-${version}-win64-debug.zip"
)

# A release holding two of the three artifacts is worse than one holding none,
# because it looks complete. Checked together, before any upload.
missing=0
for artifact in "${artifacts[@]}"; do
    if [[ ! -f "$artifact" ]]; then
        echo "ERROR: packaging did not produce $artifact" >&2
        missing=1
    fi
done
if (( missing )); then
    exit 1
fi

# --clobber so a re-run after a failed upload replaces what it left behind
# rather than erroring on the half that already arrived.
gh release upload "$release_tag" "${artifacts[@]}" \
    --repo "$GITHUB_REPOSITORY" \
    --clobber
