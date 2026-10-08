.class C props(a)
.function main 0
    CreateObject C
    PushConstanti32 @sub
    JumpRelative 1
    Return
sub:
    CreateStackVariables 1
    CreateObject C
    SetVariableValue 1
    PushConstanti8 5
    Return
