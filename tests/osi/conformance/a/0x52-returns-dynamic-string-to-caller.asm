.function main 0
    PushConstanti32 @sub
    JumpRelative 0
    ConvertToInteger
    Return
sub:
    PushConstantString "12"
    PushConstantString "34"
    Add
    Return
