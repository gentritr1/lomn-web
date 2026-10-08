.function main2 0
    PushConstanti8 1
    Return
.globals g
.function main 0
    PushConstantf32 1.0
    SetVariableValue global:g
fail:
    IncrementVariable global:g
    Return
