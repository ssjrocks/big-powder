#!/usr/bin/env bash
# Builds a release of Big Powder for the GitHub release page:
#   build-release/   a Release build, sending crash and problem reports to APP_REPORTS_URL from
#                    ~/dev/app-reports/config.local (kept out of this repo)
#   release/big-powder-<version>-linux-x86_64.tar.gz   the program, its shaders, README and LICENSE
#   release/big-powder.json   version, file, sha256 and size: what the in-game updater reads
# Usage: tools/release.sh   (the version is the one in CMakeLists.txt's project())
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
version=$(sed -n 's/^project(big_powder VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$version" ] || { echo "no version in CMakeLists.txt" >&2; exit 1; }
url=""
if [ -f "$HOME/dev/app-reports/config.local" ]; then
    url=$(. "$HOME/dev/app-reports/config.local"; echo "${APP_REPORTS_URL:-}")
fi
[ -n "$url" ] || echo "warning: no APP_REPORTS_URL: this build sends no reports" >&2

cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DAPP_REPORTS_URL="$url" >/dev/null
cmake --build build-release

name="big-powder-$version"
rm -rf "release/$name"
mkdir -p "release/$name/shaders"
cp build-release/big-powder "release/$name/"
cp build-release/shaders/*.spv "release/$name/shaders/"
cp README.md LICENSE "release/$name/"
printf '{"version": "%s"}\n' "$version" >"release/$name/big-powder.json" # (marks a release install the updater may replace)
tarball="$name-linux-x86_64.tar.gz"
tar -C release --owner=0 --group=0 -czf "release/$tarball" "$name"
sha=$(sha256sum "release/$tarball" | cut -d' ' -f1)
size=$(stat -c %s "release/$tarball")
printf '{"version": "%s", "file": "%s", "sha256": "%s", "size": %s}\n' "$version" "$tarball" "$sha" "$size" \
    >release/big-powder.json
echo "release/$tarball ($size bytes)"
echo "release/big-powder.json"
