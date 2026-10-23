#!/bin/sh
# Zip the ForgeTalents addon for players: addon/pack.sh <output dir> -> <output dir>/ForgeTalents.zip
# The .toc's Version is set to the module's (FORGE_VERSION in src/forge_loader.cpp, without the "v").
# Uses python3, else cmake (a build container). wowproject: the pipeline runs it at install, into the client-patch
# dir the dashboard serves.
set -eu
out=$(cd "${1:?usage: pack.sh <output dir>}" && pwd)
cd "$(dirname "$0")"
version=$(sed -n 's/.*FORGE_VERSION = "v\{0,1\}\([^"]*\)".*/\1/p' ../src/forge_loader.cpp)
[ -n "$version" ] || { echo "pack.sh: no FORGE_VERSION in src/forge_loader.cpp" >&2; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp -R ForgeTalents "$stage/"
sed "s/^## Version:.*/## Version: $version/" ForgeTalents/ForgeTalents.toc > "$stage/ForgeTalents/ForgeTalents.toc"

tmp=$out/ForgeTalents.zip.tmp
rm -f "$tmp"
cd "$stage"
if command -v python3 >/dev/null; then
    python3 -I -c '
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir("ForgeTalents")):
        z.write(os.path.join("ForgeTalents", name))
' "$tmp"
else
    cmake -E tar cf "$tmp" --format=zip ForgeTalents
fi
mv "$tmp" "$out/ForgeTalents.zip"
echo "$out/ForgeTalents.zip (v$version)"
