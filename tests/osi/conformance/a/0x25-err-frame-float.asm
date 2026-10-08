.function main2 0
    PushConstanti8 1
    Return
.function main 0
    CreateStackVariables 1
    PushConstantf32 1.0
    SetVariableValue 0
fail:
    DecrementVariable 0
    Return
