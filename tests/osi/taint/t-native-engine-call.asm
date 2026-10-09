; t-native-engine-call: the arguments of an engine call a native makes are its outputs (they carry its tag); a Run that
; ends in Halt returns a clean nothing, whatever the nested call returned (no process-result sink)
.function inner 1
    GetVariableValue 0
b_arg:
    CompareAndBranchIfFalse @a
a:
    GetVariableValue 0
    Return
.function Process 3
    CallGameFunction t.n 0
    Pop
    Halt
