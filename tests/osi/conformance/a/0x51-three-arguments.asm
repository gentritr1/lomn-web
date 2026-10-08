.class C props(a)
.function main 0
    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    GetVariableValue 0
    PushConstanti8 10
    PushConstanti8 20
    GetVariableValue 0
    GetMemberFunction .m
    JumpAbsolute 3
    Return
.method m
    GetVariableValue 2
    Return
