#!/bin/sh
# Link the plugins of this repository into an op1r-src checkout, so its harness and make_plugin.py find them.
#   ./setup.sh [path to op1r-src]        (default: ../op1r-src, cloned if missing)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-"$HERE/../op1r-src"}
[ -d "$SRC/synth_sdk" ] || git clone https://github.com/pcppcp/op1r-src.git "$SRC"
for p in "$HERE"/*/plugin.json; do
    d=$(dirname "$p"); n=$(basename "$d")
    ln -sfn "$d" "$SRC/synth_sdk/plugins/$n"
    echo "linked $n -> $SRC/synth_sdk/plugins/$n"
done
echo "test:  cd $SRC/synth_sdk && python3 harness/audio.py monolith"
echo "build: cd $SRC/synth_sdk && python3 make_plugin.py monolith   (needs the bfin-elf toolchain, see op1r-src README)"
