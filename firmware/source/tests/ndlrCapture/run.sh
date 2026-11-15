#!/bin/sh
# Host test for core/source/NdlrCapture.cpp. Needs a Tracker CE v1.0.0 checkout for mtChordDefs.h:
#   TRACKER_CE=/path/to/tracker-ce ./run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../core/source"
work=$(mktemp -d)
cp "$src/NdlrCapture.cpp" "$src/NdlrCapture.h" "$TRACKER_CE/core/source/mtChordDefs.h" "$work/"
c++ -std=c++17 -Wall -Wextra -DTRACKER_MINI -I"$here/stubs" -I"$work" -o "$work/test" "$here/test.cpp" "$work/NdlrCapture.cpp"
"$work/test"
