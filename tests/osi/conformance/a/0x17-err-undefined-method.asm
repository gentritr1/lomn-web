.function main2 0
    PushConstanti8 1
    Return
.class C props(a)
.symbols zz
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
    PushConstanti8 77
fail:
    GetThisMemberFunction .zz
    Return
