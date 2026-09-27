#!/usr/bin/env bash
set -euo pipefail

# Release Please will not open the next release PR while a merged one is still
# untagged; it logs a warning and exits successfully. Main runs overlap, so the
# head run can get here while the release merge's own run is still tagging.
# Wait for that tag rather than letting the refresh silently do nothing.

: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"
attempts=${WAIT_ATTEMPTS:-20}
interval=${WAIT_SECONDS:-30}

for ((attempt = 1; ; attempt++)); do
    untagged=$(gh api "repos/${GITHUB_REPOSITORY}/pulls?state=closed&base=main&sort=updated&direction=desc&per_page=100" \
        --jq '.[] | select(.merged_at != null and any(.labels[]?; .name == "autorelease: pending")) | .number')
    if [[ -z "$untagged" ]]; then
        exit 0
    fi
    if ((attempt >= attempts)); then
        break
    fi
    echo "Release Please PR #${untagged//$'\n'/, #} is merged but not yet tagged; waiting for its run to tag it."
    sleep "$interval"
done

echo "ERROR: Release Please PR #${untagged//$'\n'/, #} is merged but still untagged, so the release PR cannot be refreshed." >&2
echo "Re-run the release merge's workflow run, then re-run this job." >&2
exit 1
