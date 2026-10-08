.globals g
.function main 0
    PushConstantString "a"
    PushConstantString "b"
    Add
    SetVariableValue global:g
    PushConstanti8 3
    SetVariableValue global:g
    GetVariableValue global:g
    Return
