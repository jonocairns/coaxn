#!/usr/bin/env bash
set -euo pipefail

: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must be set}"
: "${GITHUB_OUTPUT:?GITHUB_OUTPUT must be set}"

# The tag is derived from CMakeLists.txt rather than taken on trust.
version=$("$(dirname "${BASH_SOURCE[0]}")/project-version.sh")
release_tag="v${version}"

# Set only when this run created the release. A mismatch means release-please
# tagged a version CMakeLists.txt never received: the x-release-please-version
# marker has stopped matching, and the build would ship under the old number.
if [[ -n "${EXPECTED_RELEASE_TAG:-}" && "$release_tag" != "$EXPECTED_RELEASE_TAG" ]]; then
    echo "ERROR: released $EXPECTED_RELEASE_TAG, but CMakeLists.txt says $version" >&2
    echo "ERROR: check the x-release-please-version marker on the project() line" >&2
    exit 1
fi
if ! git rev-parse --verify --quiet "refs/tags/${release_tag}^{commit}" >/dev/null; then
    echo "ERROR: expected tag $release_tag does not exist" >&2
    exit 1
fi

head_sha=$(git rev-parse HEAD)
tag_sha=$(git rev-list -n 1 "$release_tag")
if [[ "$tag_sha" != "$head_sha" ]]; then
    echo "ERROR: tag $release_tag does not point to the checked commit" >&2
    exit 1
fi
if [[ -n "${EXPECTED_RELEASE_SHA:-}" && "$head_sha" != "$EXPECTED_RELEASE_SHA" ]]; then
    echo "ERROR: tag $release_tag does not point to the Release Please output SHA" >&2
    exit 1
fi
if ! gh release view "$release_tag" --repo "$GITHUB_REPOSITORY" >/dev/null; then
    echo "ERROR: expected GitHub Release $release_tag does not exist" >&2
    exit 1
fi

echo "tag_name=$release_tag" >>"$GITHUB_OUTPUT"
echo "version=$version" >>"$GITHUB_OUTPUT"
