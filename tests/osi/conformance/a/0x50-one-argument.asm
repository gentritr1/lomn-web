.function main 0
    PushConstanti8 20
    PushConstanti32 @sub
    JumpRelative 1
    Return
sub:
    GetVariableValue 0
    Return
