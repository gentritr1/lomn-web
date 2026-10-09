; n-reenter-depth3
.globals g
.function main2 0 @m2
.function inner 0 @inner
.function main 0
    PushConstant0
    SetVariableValue global:g
    PushConstanti8 11
    PushConstanti8 22
    CallGameFunction t.re 0
    Pull 3
    Return
inner:
    IncrementVariable global:g
    PushConstanti16 1000
    CallGameFunction t.re 0
    Pull 2
    Return
m2:
    PushConstanti8 9
    Return
