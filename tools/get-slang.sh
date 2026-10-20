#!/usr/bin/env bash
# Downloads the pinned Slang shader compiler release into tools/slang.
set -euo pipefail

VERSION=2026.20
here="$(cd "$(dirname "$0")" && pwd)"
dest="$here/slang"

if [[ -x "$dest/bin/slangc" ]] && "$dest/bin/slangc" -version 2>&1 | grep -q "$VERSION"; then
    echo "slang $VERSION already in $dest"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
gh release download "v$VERSION" --repo shader-slang/slang \
    --pattern "slang-$VERSION-linux-x86_64.tar.gz" --dir "$tmp"
rm -rf "$dest"
mkdir -p "$dest"
tar -xzf "$tmp/slang-$VERSION-linux-x86_64.tar.gz" -C "$dest"
"$dest/bin/slangc" -version
