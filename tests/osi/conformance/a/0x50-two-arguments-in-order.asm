.function main 0
    PushConstanti8 3
    PushConstanti8 5
    PushConstanti32 @sub
    JumpRelative 2
    Return
sub:
    GetVariableValue 1
    Return
