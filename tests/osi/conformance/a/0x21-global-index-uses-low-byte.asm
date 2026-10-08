.globals g h
.function main 0
    PushConstanti8 1
    SetVariableValue global:g
    PushConstanti8 5
    SetVariableValue global:h
    GetVariableValue 0x8001
    Return
