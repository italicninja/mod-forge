#!/bin/sh
# Zip the ForgeTalents addon for players: addon/pack.sh <output dir> -> <output dir>/ForgeTalents.zip
# (wowproject: addon/pack.sh /data/wow-wotlk/client-patch, served by the dashboard).
set -eu
out=${1:?usage: pack.sh <output dir>}
cd "$(dirname "$0")"
python3 -I -c '
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir("ForgeTalents")):
        z.write(os.path.join("ForgeTalents", name))
' "$out/ForgeTalents.zip.tmp"
mv "$out/ForgeTalents.zip.tmp" "$out/ForgeTalents.zip"
echo "$out/ForgeTalents.zip"
