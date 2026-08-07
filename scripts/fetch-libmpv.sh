#!/usr/bin/env bash
# Fetch the pinned libmpv Windows development package.
#
# The pin is an upstream mpv commit rather than a tagged release, chosen for
# artifact availability rather than for any feature the tag lacks. The build
# tag, the commit and the archive digest below are all recorded, so the runtime
# can be re-obtained byte-for-byte and checked that it is.
#
# The rationale is not restated here: see docs/packaging.md, and PRD 7.1 for
# the checks behind it.
set -euo pipefail

MPV_BUILD_TAG="20260610"
MPV_GIT_SHORT="304426c"
MPV_GIT_COMMIT="304426c390901436fb1d4a63efbd582ae80c88f4"
# The commit pins provenance; this pins content. A GitHub release asset can be
# replaced without the tag moving, and these bytes are linked into the shipped
# binary, so the archive is verified before anything is unpacked.
MPV_ARCHIVE_SHA256="8cbb25ea784f01afbb3f904217cab1317430a8bcfd5680fd827a866367f71cc9"

ARCHIVE="mpv-dev-x86_64-${MPV_BUILD_TAG}-git-${MPV_GIT_SHORT}.7z"
URL="https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/${MPV_BUILD_TAG}/${ARCHIVE}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${repo_root}/third_party/mpv"

# An already-unpacked tree is accepted without re-verification, deliberately.
# The digest below guards the point where third-party bytes enter the tree; it
# cannot be recomputed from unpacked files, so re-checking here would need a
# separate per-file manifest to maintain. Anyone who can rewrite third_party/mpv
# in place can rewrite this script too, so that manifest would buy little.
# Delete the directory to force a fetch that is verified again.
if [[ -f "${dest}/include/mpv/client.h" && -f "${dest}/libmpv.dll.a" ]]; then
    echo "libmpv already present at ${dest} (mpv ${MPV_GIT_SHORT}, not re-verified)"
    exit 0
fi

echo "Fetching libmpv ${MPV_GIT_SHORT} (mpv commit ${MPV_GIT_COMMIT})"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

curl -fsSL -o "${tmp}/${ARCHIVE}" "${URL}"

actual_sha256="$(sha256sum "${tmp}/${ARCHIVE}" | cut -d ' ' -f 1)"
if [[ "${actual_sha256}" != "${MPV_ARCHIVE_SHA256}" ]]; then
    echo "ERROR: libmpv archive does not match its pinned digest; nothing unpacked." >&2
    echo "  expected ${MPV_ARCHIVE_SHA256}" >&2
    echo "  actual   ${actual_sha256}" >&2
    echo "  source   ${URL}" >&2
    exit 1
fi
echo "Archive verified (sha256 ${MPV_ARCHIVE_SHA256})"

mkdir -p "${dest}"
7z x -y -o"${dest}" "${tmp}/${ARCHIVE}" >/dev/null

# Recorded so the packaged libmpv-PINNED.txt states what was verified, not only
# where it came from.
printf '%s\n' \
    "build_tag=${MPV_BUILD_TAG}" \
    "git_commit=${MPV_GIT_COMMIT}" \
    "archive_sha256=${MPV_ARCHIVE_SHA256}" \
    "source=${URL}" > "${dest}/PINNED.txt"

echo "libmpv unpacked to ${dest}"
ls -1 "${dest}"
