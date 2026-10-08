.class C props(a)
.function main 0
    CreateStackVariables 1
    CreateObject C
    SetVariableValue 0
    GetVariableValue 0
    GetVariableValue 0
    GetMemberFunction .m1
    JumpAbsolute 1
    Return
.method m1
    GetVariableValue 0
    GetThisMemberFunction .m2
    JumpAbsolute 1
    Return
.method m2
    PushConstanti8 44
    Return
.method m3
    PushConstanti8 55
    Return
