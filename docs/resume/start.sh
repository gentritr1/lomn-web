#!/bin/bash
# Start one headless controller run on Console credits, detached (survives closing the tab).
# Usage: start.sh <label>   (reads brief-ctl-<label>.md next to this script)
L="$1"; D="$(cd "$(dirname "$0")" && pwd)"; ROOT=~/Desktop/lomn-web
[ -f "$D/brief-ctl-$L.md" ] || { echo "no brief-ctl-$L.md"; exit 2; }
cd "$ROOT" || exit 3
nohup env -i HOME="$HOME" USER="$USER" TERM=xterm LANG=en_US.UTF-8 PATH="$PATH" \
  claude -p "$(cat "$D/brief-ctl-$L.md")" --model opus --effort medium --max-budget-usd 45 \
  --output-format json --permission-mode acceptEdits --allowedTools "Bash,Read,Edit,Write,Grep,Glob,Agent,Task" \
  --add-dir lomn-web-m2 > "$D/ctl-$L.result.json" 2> "$D/ctl-$L.stderr.log" < /dev/null &
echo "started pid $!"
