; n-swallow-continue
.globals g
.function main2 0 @m2
.function onevent 0 @ev
.function main 0
    PushConstanti8 1
    SetVariableValue global:g
c:
    CallGameFunction t.sw 0
after_c:
    Pop
    PushConstanti8 2
    SetVariableValue global:g
    PushConstanti8 1
    Return
ev:
    DebugOn
    PushConstanti8 7
    Return
m2:
    PushConstanti8 9
    Return
