# ctest helper (M2a-2 T3 fix 1): runner rules of lomn_osi_conform that no committed corpus program pins, on a temporary
# corpus built here (natively, and under node when RUNNER is set). Expectations derived by hand in the comments.
#  r/depth-after-swallow (must PASS): calls.depth compares context_depth, conformance.md §1's literal meaning (controller
#    ruling 2(b)), which a D16 swallow leaves one above the engine calls in flight. main (context_depth 1) sets g = 0 and
#    calls t.sw, which swallows bad's DebugOn: bad's marker stays (context_depth 2, 1 call in flight) and dispatch goes on
#    in bad at @bad+1. bad calls t.re (depth 3): context_depth 2 < 3 -> inner (context_depth 3): g = 1; inner's t.re:
#    3 is not < 3 -> no call, 7. inner returns 7 (disposed by the host), t.re returns 7, bad's Return ends main's Run at
#    bad's marker: result 7, depth 1 (main's marker), context_depth 1, pc = that marker's value = @after_sw. g = 1. An
#    in-flight reading would call inner twice (1 < 3, then 2 < 3): g = 2.
#  r/then-after-raise (must FAIL on `ok` only): the first call raises against an `expect`; rule 19 resets before `then`
#    (review M-2), so then.depth 0 / then.context_depth 0 hold. Without the reset they would be 1 (main's marker left).
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT}/c/r)
file(WRITE ${OUT}/c/r/depth-after-swallow.asm ".globals g
.function bad 0 @bad
.function inner 0 @inner
.function main 0
    PushConstant0
    SetVariableValue global:g
    CallGameFunction t.sw 0
after_sw:
    Return
bad:
    DebugOn
    CallGameFunction t.re 0
    Return
inner:
    IncrementVariable global:g
    CallGameFunction t.re 0
    Return
")
file(WRITE ${OUT}/c/r/depth-after-swallow.expect.json "{\"id\":\"depth-after-swallow\",\"entry\":\"main\",\"args\":[],\"registry\":\"none\",
 \"natives\":[{\"cls\":\"t\",\"name\":\"sw\",\"min\":0,\"max\":0,\"types\":[],\"returns\":{\"type\":\"0x10\",\"value\":5},\"swallow\":true,\"calls\":{\"entry\":\"bad\",\"args\":[]}},
              {\"cls\":\"t\",\"name\":\"re\",\"min\":0,\"max\":0,\"types\":[],\"returns\":{\"type\":\"0x10\",\"value\":7},\"calls\":{\"entry\":\"inner\",\"args\":[],\"depth\":3}}],
 \"expect\":{\"ok\":true,\"result\":{\"type\":\"0x10\",\"value\":7},\"depth\":1,\"context_depth\":1,\"fpu_pc\":64,\"pc_after\":\"@after_sw\",
             \"globals\":{\"g\":{\"type\":\"0x10\",\"value\":1}},
             \"swallowed\":[{\"message\":\"Debugger not enabled! but encountered DebugOn\",\"pc\":\"@bad\",\"op\":\"0x02\",\"depth\":2}]}}")
file(WRITE ${OUT}/c/r/then-after-raise.asm ".function main2 0 @m2
.function main 0
    DebugOn
    Return
m2:
    PushConstanti8 9
    Return
")
file(WRITE ${OUT}/c/r/then-after-raise.expect.json "{\"id\":\"then-after-raise\",\"entry\":\"main\",\"args\":[],\"natives\":[],\"registry\":\"none\",
 \"expect\":{\"ok\":true},
 \"then\":{\"entry\":\"main2\",\"args\":[],\"ok\":true,\"result\":{\"type\":\"0x10\",\"value\":9},\"depth\":0,\"context_depth\":0}}")
execute_process(COMMAND ${PYTHON} ${ASSEMBLE} ${OUT}/c ${OUT}/c-osi RESULT_VARIABLE arc OUTPUT_VARIABLE aout ERROR_VARIABLE aerr TIMEOUT 60)
if(NOT arc EQUAL 0)
  message(FATAL_ERROR "conform_assemble failed: ${aout}${aerr}")
endif()
execute_process(COMMAND ${RUNNER} ${EXE} ${OUT}/c --osi-dir ${OUT}/c-osi RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
set(out "${out}${err}")
message(STATUS "${out}")
foreach(line "conformance: 1/2 pass, 0 pending")
  string(FIND "\n${out}" "\n${line}\n" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "missing the line '${line}' in:\n${out}")
  endif()
endforeach()
if(out MATCHES "FAIL r/depth-after-swallow")
  message(FATAL_ERROR "calls.depth after a swallow (context_depth reading) failed:\n${out}")
endif()
if(NOT out MATCHES "FAIL r/then-after-raise.expect.json: ok: ")
  message(FATAL_ERROR "the raising first call was not reported:\n${out}")
endif()
if(out MATCHES "FAIL r/then-after-raise.expect.json: then\\.")
  message(FATAL_ERROR "`then` ran without reset_after_error after a raising first call (rule 19):\n${out}")
endif()
