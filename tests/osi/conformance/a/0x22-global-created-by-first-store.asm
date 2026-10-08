.globals g
.function main 0
    PushConstantf32 2.5
    SetVariableValue global:g
    GetVariableValue global:g
    Return
