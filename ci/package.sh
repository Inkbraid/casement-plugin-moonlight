#!/usr/bin/env bash
# Puts the plugin folder together for a release zip or a CI artifact:
#
#   ci/package.sh <build dir> <vcpkg triplet dir> <output dir>
#
# <output dir>/moonlight/ gets the exe, the files Casement reads and the
# licence of every library linked into the exe. The vcpkg triplet dir
# (e.g. vcpkg/installed/x64-windows-static-release) has the licences of
# the libraries vcpkg built.
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <build dir> <vcpkg triplet dir> <output dir>" >&2
    exit 2
fi
build=$1
libs=$2
out=$3
root=$(cd "$(dirname "$0")/.." && pwd)
dest="$out/moonlight"

rm -rf "$dest"
mkdir -p "$dest/licenses"
cp "$build/casement-moonlight.exe" "$dest/"
for f in plugin.json icon.png LICENSE README.md CHANGELOG.md; do cp "$root/$f" "$dest/"; done

common="$root/third_party/moonlight-common-c"
cp "$common/LICENSE.txt" "$dest/licenses/moonlight-common-c.txt"
cp "$common/enet/LICENSE" "$dest/licenses/enet.txt"
cp "$common/nanors/LICENSE" "$dest/licenses/nanors.txt"
cp "$root/third_party/pugixml/LICENSE.md" "$dest/licenses/pugixml.txt"
cp "$root/third_party/nlohmann/LICENSE.MIT" "$dest/licenses/nlohmann-json.txt"
for port in ffmpeg opus openssl; do
    cp "$libs/share/$port/copyright" "$dest/licenses/$port.txt"
done

echo "packaged $dest:"
(cd "$dest" && find . -type f | sort)
