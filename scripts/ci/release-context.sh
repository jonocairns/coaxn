#!/usr/bin/env bash
set -euo pipefail

: "${EXPECTED_SHA:?EXPECTED_SHA must be set}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"
: "${GITHUB_OUTPUT:?GITHUB_OUTPUT must be set}"

if [[ ! "$EXPECTED_SHA" =~ ^[0-9a-f]{40}$ ]]; then
    echo "ERROR: invalid expected commit SHA: $EXPECTED_SHA" >&2
    exit 1
fi

main_sha=$(gh api "repos/${GITHUB_REPOSITORY}/git/ref/heads/main" --jq .object.sha)
if [[ "$main_sha" == "$EXPECTED_SHA" ]]; then
    echo "update-pr=true" >>"$GITHUB_OUTPUT"
else
    echo "update-pr=false" >>"$GITHUB_OUTPUT"
    echo "main advanced to $main_sha; its own CI run will update the release PR."
fi

pending_release_pr=$(gh api "repos/${GITHUB_REPOSITORY}/commits/${EXPECTED_SHA}/pulls" \
    --jq ".[] | select(.base.ref == \"main\" and .merge_commit_sha == \"${EXPECTED_SHA}\" and any(.labels[]?; .name == \"autorelease: pending\")) | .number")
tagged_release_pr=$(gh api "repos/${GITHUB_REPOSITORY}/commits/${EXPECTED_SHA}/pulls" \
    --jq ".[] | select(.base.ref == \"main\" and .merge_commit_sha == \"${EXPECTED_SHA}\" and any(.labels[]?; .name == \"autorelease: tagged\")) | .number")

if [[ -n "$pending_release_pr" ]]; then
    echo "create-release=true" >>"$GITHUB_OUTPUT"
    echo "release-context=true" >>"$GITHUB_OUTPUT"
    echo "Commit $EXPECTED_SHA merged pending Release Please PR #$pending_release_pr."
elif [[ -n "$tagged_release_pr" ]]; then
    echo "create-release=false" >>"$GITHUB_OUTPUT"
    echo "release-context=true" >>"$GITHUB_OUTPUT"
    echo "Commit $EXPECTED_SHA merged tagged Release Please PR #$tagged_release_pr; recovering downstream release work."
else
    echo "create-release=false" >>"$GITHUB_OUTPUT"
    echo "release-context=false" >>"$GITHUB_OUTPUT"
fi
