#!/bin/bash
set -e
cd "$(dirname "$0")/work"; source tools/env.sh
V=./build/native/lomn_view; I="$LOMN_DATA/install"
run() { timeout 120 $V --install "$I" --level $1 --area $2 --out ../out/look/$2_far$3.png --far $3 ${4:+--yaw $4} >/dev/null; }
for far in 4000 100000; do run lev1 vllg $far; run lev2 ghut $far; run lev4 crss $far 67.5; done
for a in vllg ghut crss; do echo -n "$a far 4000 vs 1e5: "; python3 ../pngtool.py diff ../out/look/${a}_far4000.png ../out/look/${a}_far100000.png; done
