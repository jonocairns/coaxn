#!/usr/bin/env bash
set -euo pipefail

# Usage: check-release-artifacts.sh [version]
# Prints the artifact paths, one per line, when all of them exist. The version
# defaults to the one in CMakeLists.txt.
script_dir=$(dirname "${BASH_SOURCE[0]}")
version=${1:-$("$script_dir/project-version.sh")}

artifacts=(
    "build/coax-${version}-win64-setup.exe"
    "build/coax-${version}-win64.zip"
    "build/coax-${version}-win64-debug.zip"
)

# A release holding two of the three artifacts is worse than one holding none,
# because it looks complete. Checked together, before anything is uploaded.
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
printf '%s\n' "${artifacts[@]}"
