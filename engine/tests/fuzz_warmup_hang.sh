#!/bin/sh
# ctest fuzz_formats_warmup_hang (M1a-4 cleanup C4): a hang during the seed warm-up must be named by --last-input.
# wasm32 has no per-input watchdog, so after an outer timeout the --last-input file is the only record of the input
# that hung. --test-hang-seed 1 makes the driver spin forever on synthetic .x seed #1 of the warm-up; the file must
# name exactly that seed while the driver is stuck there. The driver is then killed (as the make target's timeout
# would). Waits on the file's content, never on a fixed time; gives up after ~60 s (a failure).
# usage: fuzz_warmup_hang.sh DRIVER LAST_INPUT_FILE [EMULATOR...]
drv=$1
last=$2
shift 2
/bin/rm -f "$last"
"$@" "$drv" --smoke --formats x --n 1 --last-input "$last" --test-hang-seed 1 &
pid=$!
want='^x  *seed #1 (synthetic, unmutated) *$'
i=0
while [ $i -lt 600 ]; do
  if grep -q "$want" "$last" 2>/dev/null; then
    alive=yes
    kill -0 $pid 2>/dev/null || alive=no
    kill $pid 2>/dev/null
    wait $pid 2>/dev/null
    rc=$?
    echo "last input: $(cat "$last") (driver still running when read: $alive)"
    [ $alive = yes ] && exit 0
    # Natively the 10 s CPU watchdog may have ended the spin first (exit 4, "FINDING x seed #1 ... (hang)").
    [ $rc -eq 4 ] && exit 0
    echo "FAIL: the driver had exited (rc=$rc); --test-hang-seed should have kept it in the warm-up"
    exit 1
  fi
  if ! kill -0 $pid 2>/dev/null; then
    wait $pid
    echo "FAIL: driver exited (rc=$?) and the last-input file does not name seed #1: '$(cat "$last" 2>/dev/null)'"
    exit 1
  fi
  sleep 0.1
  i=$((i + 1))
done
kill $pid 2>/dev/null
wait $pid 2>/dev/null
echo "FAIL: after ~60 s the last-input file does not name the hanging seed: '$(cat "$last" 2>/dev/null)'"
exit 1
