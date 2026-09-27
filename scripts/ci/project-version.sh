#!/usr/bin/env bash
set -euo pipefail

# CPack names the artifacts from this line and the update check compiles it into
# the binary, so every release script takes the version from here.
version=$(sed -n 's/^project(coax_native VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
if [[ -z "$version" ]]; then
    echo "ERROR: no project(coax_native VERSION ...) line in CMakeLists.txt" >&2
    exit 1
fi
echo "$version"
