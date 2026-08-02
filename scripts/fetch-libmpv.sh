#!/usr/bin/env bash
# Fetch the pinned libmpv Windows development package.
#
# The pin is a specific upstream mpv commit, not a tagged release: the
# composition presentation path this project depends on landed after v0.41.0.
# Both the build tag and the commit are recorded so the runtime can be
# re-obtained byte-for-byte.
set -euo pipefail

MPV_BUILD_TAG="20260610"
MPV_GIT_SHORT="304426c"
MPV_GIT_COMMIT="304426c390901436fb1d4a63efbd582ae80c88f4"

ARCHIVE="mpv-dev-x86_64-${MPV_BUILD_TAG}-git-${MPV_GIT_SHORT}.7z"
URL="https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/${MPV_BUILD_TAG}/${ARCHIVE}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${repo_root}/third_party/mpv"

if [[ -f "${dest}/include/mpv/client.h" && -f "${dest}/libmpv.dll.a" ]]; then
    echo "libmpv already present at ${dest} (mpv ${MPV_GIT_SHORT})"
    exit 0
fi

echo "Fetching libmpv ${MPV_GIT_SHORT} (mpv commit ${MPV_GIT_COMMIT})"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

curl -fsSL -o "${tmp}/${ARCHIVE}" "${URL}"
mkdir -p "${dest}"
7z x -y -o"${dest}" "${tmp}/${ARCHIVE}" >/dev/null

printf '%s\n' \
    "build_tag=${MPV_BUILD_TAG}" \
    "git_commit=${MPV_GIT_COMMIT}" \
    "source=${URL}" > "${dest}/PINNED.txt"

echo "libmpv unpacked to ${dest}"
ls -1 "${dest}"
