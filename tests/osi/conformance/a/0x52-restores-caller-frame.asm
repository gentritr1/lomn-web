.function main 0
    CreateStackVariables 1
    PushConstanti8 9
    SetVariableValue 0
    PushConstanti32 @sub
    JumpRelative 0
    Pop
    GetVariableValue 0
    Return
sub:
    CreateStackVariables 2
    PushConstanti8 1
    Return
