# ctest helper (spec M1 A9): render the synthetic STATE scene (--synthetic 2) on the null backend twice with --state-log
# and require both D3D8 state logs to be non-empty, byte-identical, and equal to the checked-in EXPECTED log: device
# init (CreateDirectXWindow / AppMain) + the FIRST frame, whose notes read "# frame 0" (SageFrame counts from 0).
# Task 8B E1: then once with --frames 2, equal to EXPECTED_F2: the same + the second, steady-state frame ("# frame 1":
# render states carried over from frame 1's HUD view, e.g. ALPHAREF 0 -> 1 again, and a projection that is no longer
# dirty, so it is set once instead of twice). RUNNER is empty natively and "node" for wasm32, so both are pinned to the
# same files (make test-wasm also cmp's the native and wasm32 outputs directly).
foreach(run a b)
  file(MAKE_DIRECTORY ${OUT}/${run})
  execute_process(COMMAND ${RUNNER} ${EXE} --synthetic 2 --backend null --out ${OUT}/${run}/calls.txt --state-log ${OUT}/${run}/state.txt
                  RESULT_VARIABLE rc TIMEOUT 120)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lomn_view --synthetic 2 run ${run} failed: ${rc}")
  endif()
  file(SIZE ${OUT}/${run}/state.txt sz)
  if(sz EQUAL 0)
    message(FATAL_ERROR "${OUT}/${run}/state.txt is empty")
  endif()
endforeach()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/a/state.txt ${OUT}/b/state.txt RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
  message(FATAL_ERROR "A9 state log differs between two runs")
endif()
if(NOT EXISTS ${EXPECTED})
  message(FATAL_ERROR "missing expected log ${EXPECTED}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/a/state.txt ${EXPECTED} RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
  message(FATAL_ERROR "A9 state log ${OUT}/a/state.txt differs from the expected ${EXPECTED}")
endif()
file(READ ${OUT}/a/calls.txt.json m)
if(NOT m MATCHES "\"draws_last_frame\":21,")
  message(FATAL_ERROR "A9 metrics: expected 21 draws in the frame: ${m}")
endif()
if(NOT m MATCHES "\"lit_draws\":0,")
  message(FATAL_ERROR "A9 metrics: area nodes must draw unlit: ${m}")
endif()
# Task 8B E1: the steady state (--frames 2).
file(MAKE_DIRECTORY ${OUT}/f2)
execute_process(COMMAND ${RUNNER} ${EXE} --synthetic 2 --backend null --frames 2 --out ${OUT}/f2/calls.txt --state-log ${OUT}/f2/state.txt
                RESULT_VARIABLE rc TIMEOUT 120)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lomn_view --synthetic 2 --frames 2 failed: ${rc}")
endif()
if(NOT EXISTS ${EXPECTED_F2})
  message(FATAL_ERROR "missing expected log ${EXPECTED_F2}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/f2/state.txt ${EXPECTED_F2} RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
  message(FATAL_ERROR "A9 steady-state log ${OUT}/f2/state.txt differs from the expected ${EXPECTED_F2}")
endif()
