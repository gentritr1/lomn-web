# M2a-3 T4a: corpus group h (host events, Z blocks; docs/osi/conformance.md §6 rule 23) on the C++ runner. Since the T4a+T4b
# merge the three host-source programs carry no pending mark; since T6 (the HUD model) h-z-hud runs too: all four, none pending.
# RUNNER is empty natively and "node" for wasm32; ASSEMBLE is tools/osi/conform_assemble.py; CORPUS the h directory.
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
file(COPY ${CORPUS}/ DESTINATION ${OUT}/h)
execute_process(COMMAND ${PYTHON} ${ASSEMBLE} ${OUT} ${OUT}-osi RESULT_VARIABLE arc OUTPUT_VARIABLE aout ERROR_VARIABLE aerr
                TIMEOUT 60)
if(NOT arc EQUAL 0)
  message(FATAL_ERROR "conform_assemble failed: ${aout}${aerr}")
endif()
execute_process(COMMAND ${RUNNER} ${EXE} ${OUT} --osi-dir ${OUT}-osi --trace-dir ${OUT}-trace
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
message(STATUS "${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "group h: expected exit 0, got ${rc}:\n${out}${err}")
endif()
foreach(line "conformance: 4/4 pass, 0 pending")
  string(FIND "\n${out}" "\n${line}\n" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "missing the line '${line}' in:\n${out}")
  endif()
endforeach()
