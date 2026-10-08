.globals g
.function main 0
    PushConstanti8 5
    SetVariableValue global:g
    DecrementVariable global:g
    GetVariableValue global:g
    Return
