; h-z-ref: engine references after H events, seen in the Z block
.globals s
.function process 3 @proc
.function main 0
    CallGameFunction t.make 0       ; handle 1, engine 0, total 1
    Dup                             ; total 2
    CallGameFunction t.hold 1       ; H +1: engine 1, total 3; the pop disposes the argument: 2
    Pop                             ; t.hold's nothing
    Pop                             ; the last script variant of handle 1: total 1 (engine only)
    CallGameFunction t.held2 0      ; handle 2, engine 2, total 3
    Dup                             ; 4
    CallGameFunction t.drop 1       ; H -1: engine 1, total 3; the pop: 2
    Pop                             ; nothing
    SetVariableValue global:s       ; the variant moves into s: total 2
    PushConstanti8 1
    Return
proc:
    GetVariableValue global:s       ; +1
    CallGameFunction t.hold 1       ; H +1: engine 2; the pop: -1
    Pop
    GetVariableValue global:s
    CallGameFunction t.hold 1       ; H +1: engine 3
    Pop
    PushConstanti8 1
    Return
