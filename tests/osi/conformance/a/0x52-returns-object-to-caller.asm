.class C props(a)
.function main 0
    PushConstanti32 @sub
    JumpRelative 0
    GetMemberValue .a
    Return
sub:
    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    PushConstanti8 77
    GetVariableValue 0
    SetMemberValue .a
    GetVariableValue 0
    Return
