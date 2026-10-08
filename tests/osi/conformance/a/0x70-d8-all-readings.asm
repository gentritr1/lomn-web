.globals g1 g2 g3
.function main 0
    PushConstanti32 16777217
    PushConstantf32 1.0e-15
    Add
    SetVariableValue global:g1
    PushConstantString "0.5"
    ConvertToFloat
    Pop
    PushConstanti32 16777217
    PushConstantf32 1.0e-15
    Add
    SetVariableValue global:g2
    PushConstanti32 16777217
    PushConstantf32 1.0e-10
    Add
    SetVariableValue global:g3
    GetVariableValue global:g3
    Return
