#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test_root=$(mktemp -d)
trap 'rm -rf "$test_root"' EXIT

context_script="$repo_root/scripts/ci/release-context.sh"
verify_script="$repo_root/scripts/ci/verify-release-artifact.sh"
upload_script="$repo_root/scripts/ci/upload-release-asset.sh"
check_script="$repo_root/scripts/ci/check-release-artifacts.sh"
wait_script="$repo_root/scripts/ci/wait-for-release-tag.sh"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

assert_line() {
    local expected=$1
    local output_file=$2
    grep -Fqx "$expected" "$output_file" || fail "missing output: $expected"
}

# Apply the calling script's own --jq filter to a representative response, so
# the predicates that decide what the release pipeline does are what gets
# tested.
filter_response() {
    local response=$1 filter='' previous='' argument
    shift
    for argument in "$@"; do
        [[ "$previous" == --jq ]] && filter=$argument
        previous=$argument
    done
    [[ -n "$filter" ]] || fail "gh api call omitted --jq: $*"
    jq -r "$filter" <<<"$response"
}

gh() {
    case "$1 ${2:-}" in
        "api "*)
            if [[ "$*" == *'/git/ref/heads/main'* ]]; then
                printf '%s\n' "$MOCK_MAIN_SHA"
            elif [[ "$*" == *'/commits/'*'/pulls'* ]]; then
                filter_response "${MOCK_PULLS:-[]}" "$@"
            elif [[ "$*" == *'/pulls?state=closed'* ]]; then
                filter_response "$(cat "$MOCK_CLOSED_PULLS_FILE")" "$@"
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
export -f gh fail filter_response

# Stands in for the release merge's run tagging its PR while the head run waits.
sleep() {
    [[ -z "${MOCK_TAGGED_PULLS:-}" ]] || printf '%s' "$MOCK_TAGGED_PULLS" >"$MOCK_CLOSED_PULLS_FILE"
}
export -f sleep

sha=1111111111111111111111111111111111111111
other_sha=2222222222222222222222222222222222222222
context_output="$test_root/context-output"

# One pull request as GET /repos/{owner}/{repo}/commits/{sha}/pulls returns it,
# trimmed to the fields the filter reads.
pull() {
    local number=$1 base=$2 merge_sha=$3 label=${4:-}
    local labels='[]'
    [[ -n "$label" ]] && labels=$(printf '[{"name":"%s"}]' "$label")
    printf '{"number":%s,"base":{"ref":"%s"},"merge_commit_sha":"%s","labels":%s}' \
        "$number" "$base" "$merge_sha" "$labels"
}

run_context() {
    : >"$context_output"
    EXPECTED_SHA=$sha \
        GITHUB_REPOSITORY=jonocairns/coaxn \
        GITHUB_OUTPUT=$context_output \
        MOCK_MAIN_SHA=${MOCK_MAIN_SHA:-$sha} \
        MOCK_PULLS=${MOCK_PULLS:-[]} \
        "$context_script" >/dev/null
}

MOCK_PULLS='[]' run_context
assert_line 'update-pr=true' "$context_output"
assert_line 'create-release=false' "$context_output"
assert_line 'release-context=false' "$context_output"

MOCK_PULLS="[$(pull 42 main "$sha" 'autorelease: pending')]" run_context
assert_line 'create-release=true' "$context_output"
assert_line 'release-context=true' "$context_output"

MOCK_PULLS="[$(pull 42 main "$sha" 'autorelease: tagged')]" run_context
assert_line 'create-release=false' "$context_output"
assert_line 'release-context=true' "$context_output"

# A commit on main can belong to other PRs too; only the one it merged counts.
MOCK_PULLS="[$(pull 41 main "$other_sha" 'autorelease: pending'),$(pull 42 main "$sha")]" run_context
assert_line 'create-release=false' "$context_output"
assert_line 'release-context=false' "$context_output"

for label in 'autorelease: pending' 'autorelease: tagged'; do
    MOCK_PULLS="[$(pull 42 main "$other_sha" "$label")]" run_context
    assert_line 'create-release=false' "$context_output"
    assert_line 'release-context=false' "$context_output"

    MOCK_PULLS="[$(pull 42 develop "$sha" "$label")]" run_context
    assert_line 'create-release=false' "$context_output"
    assert_line 'release-context=false' "$context_output"
done

MOCK_PULLS="[$(pull 42 main "$sha")]" run_context
assert_line 'create-release=false' "$context_output"
assert_line 'release-context=false' "$context_output"

MOCK_MAIN_SHA=$other_sha run_context
assert_line 'update-pr=false' "$context_output"

if EXPECTED_SHA=invalid GITHUB_REPOSITORY=owner/repo GITHUB_OUTPUT="$context_output" \
    "$context_script" >/dev/null 2>&1; then
    fail 'release context accepted an invalid commit SHA'
fi

# One pull request as GET /repos/{owner}/{repo}/pulls?state=closed returns it,
# trimmed to the fields the filter reads.
closed_pull() {
    local number=$1 merged_at=$2 label=$3
    printf '{"number":%s,"merged_at":%s,"labels":[{"name":"%s"}]}' "$number" "$merged_at" "$label"
}

closed_pulls_file="$test_root/closed-pulls"
run_wait() {
    printf '%s' "$1" >"$closed_pulls_file"
    GITHUB_REPOSITORY=owner/repo \
        MOCK_CLOSED_PULLS_FILE=$closed_pulls_file \
        MOCK_TAGGED_PULLS=${MOCK_TAGGED_PULLS:-} \
        WAIT_ATTEMPTS=3 \
        "$wait_script" >/dev/null 2>&1
}

merged='"2026-09-27T00:00:00Z"'
run_wait "[$(closed_pull 42 "$merged" 'autorelease: tagged')]" \
    || fail 'release tag wait blocked on a tagged release PR'
run_wait "[$(closed_pull 42 null 'autorelease: pending')]" \
    || fail 'release tag wait blocked on an unmerged release PR'
MOCK_TAGGED_PULLS="[$(closed_pull 42 "$merged" 'autorelease: tagged')]" \
    run_wait "[$(closed_pull 42 "$merged" 'autorelease: pending')]" \
    || fail 'release tag wait did not see the release PR get tagged'
if run_wait "[$(closed_pull 42 "$merged" 'autorelease: pending')]"; then
    fail 'release tag wait gave up on an untagged release PR without failing'
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

# CI calls the check with no version, so it reads CMakeLists.txt.
printf 'project(coax_native VERSION 1.4.0 LANGUAGES CXX) # x-release-please-version\n' \
    >"$upload_dir/CMakeLists.txt"
(cd "$upload_dir" && "$check_script" >/dev/null) \
    || fail 'artifact check rejected a complete package set'
rm "$upload_dir/build/coax-1.4.0-win64-setup.exe"
if (cd "$upload_dir" && "$check_script" >/dev/null 2>&1); then
    fail 'artifact check accepted a package set without the installer'
fi

echo 'Release script tests passed'
