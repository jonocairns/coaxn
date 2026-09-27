#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test_root=$(mktemp -d)
trap 'rm -rf "$test_root"' EXIT

context_script="$repo_root/scripts/ci/release-context.sh"
verify_script="$repo_root/scripts/ci/verify-release-artifact.sh"
upload_script="$repo_root/scripts/ci/upload-release-asset.sh"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

assert_line() {
    local expected=$1
    local output_file=$2
    grep -Fqx "$expected" "$output_file" || fail "missing output: $expected"
}

gh() {
    case "$1 ${2:-}" in
        "api "*)
            if [[ "$*" == *'/git/ref/heads/main'* ]]; then
                printf '%s\n' "$MOCK_MAIN_SHA"
            elif [[ "$*" == *'autorelease: pending'* ]]; then
                printf '%s' "${MOCK_PENDING_PR:-}"
            elif [[ "$*" == *'autorelease: tagged'* ]]; then
                printf '%s' "${MOCK_TAGGED_PR:-}"
            else
                fail "unexpected gh api call: $*"
            fi
            ;;
        "release view")
            [[ "${MOCK_RELEASE_EXISTS:-true}" == true ]] || return 1
            ;;
        "release upload")
            printf '%s\n' "$*" >>"$MOCK_GH_LOG"
            ;;
        *)
            fail "unexpected gh call: $*"
            ;;
    esac
}
export -f gh fail

sha=1111111111111111111111111111111111111111
other_sha=2222222222222222222222222222222222222222
context_output="$test_root/context-output"

run_context() {
    : >"$context_output"
    EXPECTED_SHA=$sha \
        GITHUB_REPOSITORY=jonocairns/coaxn \
        GITHUB_OUTPUT=$context_output \
        MOCK_MAIN_SHA=${MOCK_MAIN_SHA:-$sha} \
        MOCK_PENDING_PR=${MOCK_PENDING_PR:-} \
        MOCK_TAGGED_PR=${MOCK_TAGGED_PR:-} \
        "$context_script" >/dev/null
}

MOCK_MAIN_SHA=$sha MOCK_PENDING_PR='' MOCK_TAGGED_PR='' run_context
assert_line 'update-pr=true' "$context_output"
assert_line 'create-release=false' "$context_output"
assert_line 'release-context=false' "$context_output"

MOCK_MAIN_SHA=$sha MOCK_PENDING_PR=42 MOCK_TAGGED_PR='' run_context
assert_line 'create-release=true' "$context_output"
assert_line 'release-context=true' "$context_output"

MOCK_MAIN_SHA=$sha MOCK_PENDING_PR='' MOCK_TAGGED_PR=42 run_context
assert_line 'create-release=false' "$context_output"
assert_line 'release-context=true' "$context_output"

MOCK_MAIN_SHA=$other_sha MOCK_PENDING_PR='' MOCK_TAGGED_PR='' run_context
assert_line 'update-pr=false' "$context_output"

if EXPECTED_SHA=invalid GITHUB_REPOSITORY=owner/repo GITHUB_OUTPUT="$context_output" \
    "$context_script" >/dev/null 2>&1; then
    fail 'release context accepted an invalid commit SHA'
fi

verify_repo="$test_root/verify"
mkdir -p "$verify_repo"
git -C "$verify_repo" init -q
git -C "$verify_repo" config user.name 'Release Test'
git -C "$verify_repo" config user.email release-test@example.com
printf 'project(coax_native VERSION 1.4.0 LANGUAGES CXX) # x-release-please-version\n' \
    >"$verify_repo/CMakeLists.txt"
git -C "$verify_repo" add CMakeLists.txt
git -C "$verify_repo" commit -qm 'chore(main): release 1.4.0'
git -C "$verify_repo" tag v1.4.0
verify_sha=$(git -C "$verify_repo" rev-parse HEAD)
verify_output="$test_root/verify-output"

run_verify() {
    : >"$verify_output"
    (
        cd "$verify_repo"
        GITHUB_REPOSITORY=owner/repo \
            GITHUB_OUTPUT=$verify_output \
            MOCK_RELEASE_EXISTS=${MOCK_RELEASE_EXISTS:-true} \
            "$verify_script"
    )
}

EXPECTED_RELEASE_TAG=v1.4.0 EXPECTED_RELEASE_SHA=$verify_sha run_verify >/dev/null
assert_line 'tag_name=v1.4.0' "$verify_output"
assert_line 'version=1.4.0' "$verify_output"

# The recovery path runs without release-please outputs.
run_verify >/dev/null
assert_line 'tag_name=v1.4.0' "$verify_output"

if EXPECTED_RELEASE_TAG=v1.5.0 run_verify >/dev/null 2>&1; then
    fail 'release verification accepted a tag/CMakeLists mismatch'
fi
if EXPECTED_RELEASE_SHA=$other_sha run_verify >/dev/null 2>&1; then
    fail 'release verification accepted a commit other than the release SHA'
fi
if MOCK_RELEASE_EXISTS=false run_verify >/dev/null 2>&1; then
    fail 'release verification accepted a missing GitHub Release'
fi

# CMakeLists moved on but the tag stayed behind: v1.4.0 no longer marks HEAD.
git -C "$verify_repo" commit -qm 'fix: later change' --allow-empty
if run_verify >/dev/null 2>&1; then
    fail 'release verification accepted a tag that does not point at HEAD'
fi

printf 'project(coax_native LANGUAGES CXX)\n' >"$verify_repo/CMakeLists.txt"
if run_verify >/dev/null 2>&1; then
    fail 'release verification accepted a CMakeLists.txt without a version'
fi

upload_dir="$test_root/upload"
mkdir -p "$upload_dir/build"
touch "$upload_dir/build/coax-1.4.0-win64-setup.exe" \
    "$upload_dir/build/coax-1.4.0-win64.zip"
upload_log="$test_root/upload-log"

run_upload() {
    (
        cd "$upload_dir"
        GITHUB_REPOSITORY=owner/repo \
            MOCK_RELEASE_EXISTS=${MOCK_RELEASE_EXISTS:-true} \
            MOCK_GH_LOG=$upload_log \
            "$upload_script" v1.4.0
    )
}

if run_upload >/dev/null 2>&1; then
    fail 'release upload accepted a missing artifact'
fi
[[ ! -e "$upload_log" ]] || fail 'release upload attached a partial set of artifacts'

touch "$upload_dir/build/coax-1.4.0-win64-debug.zip"
run_upload
grep -Fqx 'release upload v1.4.0 build/coax-1.4.0-win64-setup.exe build/coax-1.4.0-win64.zip build/coax-1.4.0-win64-debug.zip --repo owner/repo --clobber' "$upload_log" \
    || fail 'release upload was not idempotent'

if MOCK_RELEASE_EXISTS=false run_upload >/dev/null 2>&1; then
    fail 'release upload accepted a missing GitHub Release'
fi

echo 'Release script tests passed'
