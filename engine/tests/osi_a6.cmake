# ctest helper (M2a-1 T4, acceptance A6): the differential run of the real Base.osi (Setup + FRAMES Process frames +
# KEYSPEC, typed stubs). RUNNER is empty natively and node for wasm32, so each build compares ITS lomn_osi_run with the
# independent Python oracle (tools/research/osi_vm.py, run only, never read by the C++ author); `make osi-diff` also cmp's
# native against wasm32 directly. Skipped (SKIP_REGULAR_EXPRESSION) without LOMN_DATA.
#   MODE=digest: C++ digest == oracle digest with its own stubs == oracle digest replaying the C++ natives.log (A9 path),
#                C++ natives.log == oracle --log-out, and lomn_osi_run --print-keys == tools/osi/keys.py.
#   MODE=window: the full trace of frames -1..2 (Setup and frames 0-2), byte for byte.
#   MODE=rerun: no game data: tests/data/osi_a6_rerun.asm drives the Process re-run / skip rule (the controller addendum);
#                both engines must give the same digest and the same pass-count line, and that line must be the one
#                computed by hand in the .asm.
if(NOT MODE STREQUAL "rerun" AND "$ENV{LOMN_DATA}" STREQUAL "")
  message(STATUS "SKIPPED (no LOMN_DATA)")
  return()
endif()
set(BASE "$ENV{LOMN_DATA}/install/data/Base.osi")
set(TABLES --natives ${DATA}/natives.json --mods ${DATA}/natives_beta150.json --types ${DATA}/types.json)
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
function(run what)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 600)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${what} failed (${rc}):\n${o}\n${e}")
  endif()
  set(last_out "${o}" PARENT_SCOPE)
endfunction()
function(same a b what)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${a} ${b} RESULT_VARIABLE d)
  if(NOT d EQUAL 0)
    message(FATAL_ERROR "A6 ${what}: ${a} and ${b} differ")
  endif()
  message(STATUS "cmp ${what}: OK")
endfunction()

if(MODE STREQUAL "digest")
  run("lomn_osi_run" ${RUNNER} ${EXE} ${BASE} --data-dir ${DATA} --frames ${FRAMES} --keys "${KEYSPEC}"
      --digest ${OUT}/cpp.digest --natives-log ${OUT}/cpp.natives.log)
  message(STATUS "C++:\n${last_out}")
  file(WRITE ${OUT}/cpp.summary "${last_out}")
  run("oracle (own stubs)" ${PY} ${ORACLE} --run ${BASE} ${TABLES} --frames ${FRAMES} --keys "${KEYSPEC}"
      --digest ${OUT}/py_stub.digest --log-out ${OUT}/py.natives.log)
  message(STATUS "Python:\n${last_out}")
  run("oracle (replay of the C++ natives.log)" ${PY} ${ORACLE} --run ${BASE} ${TABLES} --frames ${FRAMES} --keys "${KEYSPEC}"
      --replay ${OUT}/cpp.natives.log --digest ${OUT}/py_replay.digest)
  same(${OUT}/cpp.digest ${OUT}/py_stub.digest "digest C++ == Python (own stubs)")
  same(${OUT}/cpp.digest ${OUT}/py_replay.digest "digest C++ == Python (replay)")
  same(${OUT}/cpp.natives.log ${OUT}/py.natives.log "natives.log C++ == Python")
  run("print-keys" ${RUNNER} ${EXE} --print-keys --keys "${KEYSPEC}")
  file(WRITE ${OUT}/keys_cpp.txt "${last_out}")
  run("keys.py" ${PY} ${KEYSPY} "${KEYSPEC}")
  file(WRITE ${OUT}/keys_py.txt "${last_out}")
  same(${OUT}/keys_cpp.txt ${OUT}/keys_py.txt "KEYSPEC expansion C++ == keys.py")
  if(NOT last_out MATCHES "^100 Input")
    message(FATAL_ERROR "keys.py listing does not start at frame 100: ${last_out}")
  endif()
elseif(MODE STREQUAL "window")
  run("lomn_osi_run" ${RUNNER} ${EXE} ${BASE} --data-dir ${DATA} --frames ${FRAMES} --keys "${KEYSPEC}"
      --trace ${OUT}/cpp.window --trace-frames -1:2)
  run("oracle" ${PY} ${ORACLE} --run ${BASE} ${TABLES} --frames ${FRAMES} --keys "${KEYSPEC}" --trace ${OUT}/py.trace)
  # the oracle's full trace cut to the same window: the header, then everything before the K line of frame 3
  run("window cut" ${PY} -c "import sys\nout = open(sys.argv[2], 'wb')\nfor l in open(sys.argv[1], 'rb'):\n    if l.startswith(b'K      3 '): break\n    out.write(l)"
      ${OUT}/py.trace ${OUT}/py.window)
  file(SIZE ${OUT}/cpp.window sz)
  if(sz LESS 1000000)
    message(FATAL_ERROR "the window trace is implausibly small (${sz} bytes)")
  endif()
  same(${OUT}/cpp.window ${OUT}/py.window "full trace frames -1..2 C++ == Python")
elseif(MODE STREQUAL "rerun")
  run("osiasm" ${PY} ${OSIASM} ${ASM} -o ${OUT}/rerun.osi)
  run("lomn_osi_run" ${RUNNER} ${EXE} ${OUT}/rerun.osi --data-dir ${DATA} --frames 6 --keys "esc@1" --digest ${OUT}/cpp.digest)
  string(REGEX MATCH "process passes:[^\n]*" cpp_line "${last_out}")
  run("oracle" ${PY} ${ORACLE} --run ${OUT}/rerun.osi ${TABLES} --frames 6 --keys "esc@1" --digest ${OUT}/py.digest)
  string(REGEX MATCH "process passes:[^\n]*" py_line "${last_out}")
  same(${OUT}/cpp.digest ${OUT}/py.digest "re-run digest C++ == Python")
  set(want "process passes: total 27 over 6 frames, max 11, frames with re-runs 5 (first: 0:4 1:4 2:11 4:4 5:4), frames skipped 1 (first: 3)")
  if(NOT cpp_line STREQUAL want OR NOT py_line STREQUAL want)
    message(FATAL_ERROR "pass counts:\n  C++    ${cpp_line}\n  Python ${py_line}\n  want   ${want}")
  endif()
  message(STATUS "${cpp_line}")
  # T11 (E3 rep 3): the rerun also writes one perf row; it must keep every existing key and carry the two loadavg keys
  # as unsigned decimal numbers (getloadavg, 2 decimals), named as tools/harness/perf_history.py reads them.
  set(perf ${OUT}/rerun.perf.jsonl)
  file(REMOVE ${perf})
  run("lomn_osi_run --perf-out" ${RUNNER} ${EXE} ${OUT}/rerun.osi --data-dir ${DATA} --frames 6 --keys "esc@1" --perf-out ${perf})
  file(READ ${perf} row)
  foreach(k IN ITEMS scene commit frames expected_frames insns_per_frame_p50 insns_per_frame_p95 insns_samples
          natives_per_frame_p50 natives_per_frame_p95 script_ms_per_frame_p50 script_ms_per_frame_p95 stack_high_water
          setup_instructions instructions flattening_frame stop loadavg_start loadavg_end)
    if(NOT row MATCHES "\"${k}\": ")
      message(FATAL_ERROR "A6 perf row has no key ${k}: ${row}")
    endif()
  endforeach()
  foreach(k IN ITEMS loadavg_start loadavg_end)
    if(NOT row MATCHES "\"${k}\": ([0-9]+(\\.[0-9]+)?)[,}]")
      message(FATAL_ERROR "A6 perf row ${k} is not a number >= 0: ${row}")
    endif()
  endforeach()
  message(STATUS "perf row keys: OK")
  # M2a-2 T3 fix 1 (I-1): the registry listing reports the bound state; --real-natives fails closed with no group body
  run("lomn_osi_run --list-natives" ${RUNNER} ${EXE} ${OUT}/rerun.osi --data-dir ${DATA} --list-natives)
  if(NOT last_out MATCHES "\nnative gcsaver\.getmask +tier stub  bound no\n" OR NOT last_out MATCHES "\nnatives: [0-9]+ rows, 0 bound\n")
    message(FATAL_ERROR "--list-natives does not report the bound state:\n${last_out}")
  endif()
  execute_process(COMMAND ${RUNNER} ${EXE} ${OUT}/rerun.osi --data-dir ${DATA} --real-natives --frames 1
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e TIMEOUT 120)
  if(NOT rc EQUAL 2 OR NOT "${o}${e}" MATCHES "no native group binds a body")
    message(FATAL_ERROR "--real-natives with no group body must fail at startup (exit 2), got ${rc}:\n${o}${e}")
  endif()
  run("lomn_osi_run --coverage" ${RUNNER} ${EXE} ${OUT}/rerun.osi --data-dir ${DATA} --frames 1 --coverage)
  if(NOT last_out MATCHES "coverage: natives distinct [0-9]+, bound 0, calls [0-9]+")
    message(FATAL_ERROR "--coverage line missing:\n${last_out}")
  endif()
  message(STATUS "bound reporting and the fail-closed --real-natives: OK")
else()
  message(FATAL_ERROR "MODE must be digest, window or rerun")
endif()
